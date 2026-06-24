# scheduler

A small C++20 **timer-wheel scheduler** and a per-key **debouncer** built on it,
extracted from [Xapiand](https://github.com/Kronuz/Xapiand).

## What it is

`scheduler` is a 24-hour, multi-resolution timer wheel with a background thread
that runs tasks when they come due. You schedule a cancellable task at a future
wakeup time; the thread sleeps until the earliest task is due (or until adding a
task wakes it sooner), runs everything due, and goes back to sleep. Two flavors:

- `Scheduler` runs each due task **inline** on the scheduler thread. This is what
  Xapiand's logger thread uses.
- `ThreadedScheduler` **dispatches** each due task to a worker pool, so a slow
  task can't stall the wheel.

On top of `ThreadedScheduler` sits the `Debouncer`: a per-key throttle/debounce.
Rapid touches of the same key collapse into a single eventual call, with a
randomized "force window" that guarantees a forever-touched key still fires while
spreading a thundering herd of due keys across time. Xapiand drives every
debounced fsync, commit, and replication trigger through it.

It is three layers, each in one header, stacked cleanly:

```
  stash.h        a lock-free hierarchical slot store (the timer-wheel primitive; a separate library)
      ▲
  scheduler.h    a 24-hour multi-resolution timer wheel + a thread that runs due tasks
      ▲
  debouncer.h    per-key throttle / debounce on top of the scheduler
```

## How it works

`SchedulerQueue` nests four `StashSlots` levels (from the
[`stash`](https://github.com/Kronuz/stash) library) over a `StashValues` leaf
into a single 24-hour wheel keyed on nanoseconds since the steady-clock epoch:

```
  4800 × 18 s  →  36 × 500 ms  →  10 × 50 ms  →  50 × 1 ms  →  task list
       └── 4800 × 18 s = 86 400 s = exactly 24 h of horizon, at 1 ms granularity
```

A task scheduled more than ~24 h out overflows the wheel (caught and logged). The
queue exposes `peep` (earliest upcoming, no mutation), `walk` (drain everything
due now), `clean` (GC slots older than a minute), and `add`.

`BaseScheduler` is a `Thread` (from the
[`threadpool`](https://github.com/Kronuz/threadpool) library) whose loop proposes
a wakeup (30 s idle, 100 ms when tasks pend), `peep`s for anything sooner, then
sleeps on a condition variable until that time **or** an `add()` notify wakes it,
then `walk`s the due tasks and `clean`s. One mutex, one condvar, a lock-free
queue. `Scheduler` and `ThreadedScheduler` derive from it and differ only in how
they run a due task (inline vs. dispatched to the pool).

`ScheduledTask` is a CRTP, `enable_shared_from_this` task with a `wakeup_time`
and atomic created/cleared timestamps. `clear()` cancels it with a single CAS, so
a scheduled callback is cancellable — the debouncer leans on this heavily.

## Install

CMake with `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(
  scheduler
  GIT_REPOSITORY https://github.com/Kronuz/scheduler.git
  GIT_TAG        main
)
FetchContent_MakeAvailable(scheduler)

target_link_libraries(your_target PRIVATE scheduler::scheduler)
```

`scheduler` itself `FetchContent`s its two dependencies —
[`stash`](https://github.com/Kronuz/stash) (the slot store) and
[`threadpool`](https://github.com/Kronuz/threadpool) (the thread + pool) — at the
exact SHAs it was extracted against, so you get the whole stack from one
`FetchContent_Declare`. The `scheduler` target is a small `STATIC` library (it
compiles one `.cc`, `random.cc`, for the debouncer's RNG helpers), requests
`cxx_std_20`, and puts the source dirs of all three libraries on your include
path. Then:

```cpp
#include "scheduler.h"   // Scheduler, ThreadedScheduler, ScheduledTask
#include "debouncer.h"   // Debouncer, make_debouncer, ...
```

Requires C++20. On macOS it builds with AppleClang/libc++, the same toolchain
Xapiand uses.

## Usage

### A scheduled, cancellable task

`ScheduledTask` is CRTP: derive a task `Impl` with an `operator()`, and use it as
both the scheduler's and the task's template argument.

```cpp
#include "scheduler.h"
#include <memory>

struct MyTask;
using Sched = Scheduler<MyTask, ThreadPolicyType::regular>;
using Base  = ScheduledTask<Sched, MyTask, ThreadPolicyType::regular>;

struct MyTask : Base {
    void operator()() { /* runs on the scheduler thread when due */ }
};

Sched sched("SCHED");
auto t = std::make_shared<MyTask>();
sched.add(t, std::chrono::steady_clock::now() + std::chrono::milliseconds(200));

// Cancel before it fires (a single CAS); if it hasn't run yet, it never will:
t->clear();

sched.finish();   // stop the thread and join
```

`ThreadedScheduler<MyTask, policy>` is identical to construct except it also
takes a worker-name format and a thread count
(`ThreadedScheduler sched("TSCHED", "TW{:02}", 4)`), and runs due tasks on the
pool instead of inline.

### A debouncer

`make_debouncer<Key>(...)` deduces the callback's argument tuple from the
callback itself (via `callable_traits`), so you just pass a lambda:

```cpp
#include "debouncer.h"

auto deb = make_debouncer<int>(
    "DEB", "DW{:02}", /*threads*/ 2,
    [](int key, int value) { /* called once per settled key */ },
    /*throttle_time*/        0ms,
    /*debounce_timeout*/     50ms,    // a fresh key fires after this
    /*debounce_busy_timeout*/50ms,    // subsequent touches push it out by this
    /*min_force*/           150ms,    // but never past now + random(min, max)
    /*max_force*/           250ms);

for (int v = 0; v < 100; ++v) deb.debounce(1, /*key*/1, /*value*/v);
// -> the callback runs ~once for key 1, with the last value, not 100 times.
```

`make_unique_debouncer` / `make_shared_debouncer` return the same thing wrapped in
a `unique_ptr` / `shared_ptr`. `delayed_debounce(delay, key, args...)` adds a
fixed delay on top.

## API reference

### `ScheduledTask<SchedulerImpl, ScheduledTaskImpl, ThreadPolicyType policy = regular>`

CRTP base for a cancellable scheduled task.

- `operator bool()` — true while not yet cleared.
- `clear() -> bool` — cancel; the first call wins a CAS and returns true, later
  calls return false. A cleared task is skipped when it comes due.
- derive `Impl` with `void operator()()` — your task body.

### `Scheduler<ScheduledTaskImpl, ThreadPolicyType policy = regular>`

- `Scheduler(std::string name)` — starts the scheduler thread immediately.
- `add(task)` / `add(task, wakeup_time)` — schedule (`wakeup_time` clamped to not
  be in the past; the one-arg form expects `task->wakeup_time` already set).
- `finish(wait = 10) -> bool` / `join(timeout = 60s) -> bool` — stop and wait.
- runs each due task **inline** on the scheduler thread.

### `ThreadedScheduler<ScheduledTaskImpl, ThreadPolicyType policy = regular>`

Same as `Scheduler` plus a worker pool.

- `ThreadedScheduler(std::string name, const char* format, size_t num_threads)` —
  `format` is a `std::format` worker-name string (e.g. `"TW{:02}"`).
- `size()`, `running_size()`, `threadpool_size()`, `threadpool_capacity()` —
  pool state queries.
- runs each due task by **dispatching** it to the pool.

### `Debouncer<Key, Func, Tuple, policy>` and factories

Construct via the factories, which deduce `Func`/`Tuple` for you:

- `make_debouncer<Key, policy = regular>(name, format, num_threads, func, throttle_time, debounce_timeout, debounce_busy_timeout, min_force, max_force)` — returns a `Debouncer` by value.
- `make_unique_debouncer<...>(...)` — same, as `std::unique_ptr`.
- `make_shared_debouncer<...>(...)` — same, as `std::shared_ptr`.

On a `Debouncer`:

- `debounce(key, args...)` — touch a key; collapses rapid touches into one
  eventual call to `func(args...)` carrying the last arguments.
- `delayed_debounce(delay, key, args...)` — same with a fixed extra delay.

The timing model: a fresh key fires after `debounce_timeout`; each subsequent
touch pushes the call out by `debounce_busy_timeout`, but never past a randomized
force window `now + random(min_force, max_force)`, so a forever-touched key still
fires. After firing, an optional `throttle_time` floor (when `> debounce_timeout`)
keeps the key from firing again too soon.

## Tracing and coloring

`scheduler.h` and `debouncer.h` instrument themselves through four macros that
are no-ops by default, so the library builds with zero dependency on any logging
or color header, and tracing has no runtime cost:

- `L_SCHEDULER(...)` — the wheel/scheduler's own trace channel (set to `L_NOTHING`
  unless already defined).
- `L_DEBUG_HOOK(label, ...)` — a labelled debug hook.
- `L_EXC(...)` — logs an exception swallowed in a destructor.
- `L_CALL(...)` — the debouncer's call-trace channel.

The bundled `scheduler_trace.h` supplies all four as `#ifndef`-guarded no-ops.
Two ways to plug in real implementations:

1. Point `SCHEDULER_TRACE_HEADER` at a header that defines them; `scheduler.h`
   includes it instead of `scheduler_trace.h`:

   ```sh
   c++ -std=c++20 -DSCHEDULER_TRACE_HEADER='"my_trace.h"' ...
   ```

2. Define the macros before including the library headers.

**About the color identifiers.** `scheduler.h` references nine color constants
(`BROWN`, `CLEAR_COLOR`, `LIGHT_SKY_BLUE`, `DIM_GREY`, `PURPLE`, `STEEL_BLUE`,
`DODGER_BLUE`, `LIGHT_GREEN`, `FOREST_GREEN`) **only inside the arguments of
`L_SCHEDULER(...)` / `L_DEBUG_HOOK(...)`**. In the default build those macros
expand to nothing, so the preprocessor discards their arguments and the color
identifiers are never compiled — no color stubs are needed. A trace header that
turns the macros into real, string-building calls must also define those nine
identifiers (as `std::string`s that concatenate with `+`). See
[`examples/colored_trace/`](examples/colored_trace/) for a complete, runnable
override.

To recover Xapiand's exact behavior, the trace header maps the four macros back
to Xapiand's `log.h` (`L_SCHEDULER`/`L_CALL`/`L_DEBUG_HOOK`/`L_EXC`) and the nine
color names to `colors.h`.

## Build & test

```sh
cmake -B build && cmake --build build && ctest --test-dir build
```

The test schedules tasks at future wakeup times and verifies they fire in wakeup
order on both a `Scheduler` and a `ThreadedScheduler`, that `clear()` cancels a
task, and that a `Debouncer` collapses a burst of rapid same-key calls into a
single call carrying the last arguments. It prints `all scheduler tests passed`
and exits 0. The scheduler is timing-based; the test uses generous windows, not
tight races.

## Provenance

Extracted from [Xapiand](https://github.com/Kronuz/Xapiand), where this engine
underpins the logger's dedicated background thread and every debounced fsync,
commit, and replication trigger. The standalone delta is pure decoupling:
Xapiand's `log.h` / `colors.h` macros were replaced by the injectable trace
header, and the three `random` helpers the debouncer needs were vendored into
`random.cc`/`random.hh`. The scheduler logic is otherwise identical. See
[ARCHITECTURE.md](ARCHITECTURE.md) for the design and [AGENTS.md](AGENTS.md) for
the repo map and invariants.

## License

MIT, Copyright (c) 2015-2019 Dubalu LLC. See [LICENSE](LICENSE).
