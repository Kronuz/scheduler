# scheduler API reference

## `ScheduledTask<SchedulerImpl, ScheduledTaskImpl, ThreadPolicyType policy = regular>`

CRTP base for a cancellable scheduled task.

- `operator bool()` — true while not yet cleared.
- `clear() -> bool` — cancel; the first call wins a CAS and returns true, later
  calls return false. A cleared task is skipped when it comes due.
- derive `Impl` with `void operator()()` — your task body.

## `Scheduler<ScheduledTaskImpl, ThreadPolicyType policy = regular>`

- `Scheduler(std::string name)` — starts the scheduler thread immediately.
- `add(task)` / `add(task, wakeup_time)` — schedule (`wakeup_time` clamped to not
  be in the past; the one-arg form expects `task->wakeup_time` already set).
- `finish(wait = 10) -> bool` / `join(timeout = 60s) -> bool` — stop and wait.
- runs each due task **inline** on the scheduler thread.

## `ThreadedScheduler<ScheduledTaskImpl, ThreadPolicyType policy = regular>`

Same as `Scheduler` plus a worker pool.

- `ThreadedScheduler(std::string name, const char* format, size_t num_threads)` —
  `format` is a `std::format` worker-name string (e.g. `"TW{:02}"`).
- `size()`, `running_size()`, `threadpool_size()`, `threadpool_capacity()` —
  pool state queries.
- runs each due task by **dispatching** it to the pool.

## `Debouncer<Key, Func, Tuple, policy>` and factories

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
[`examples/colored_trace/`](../examples/colored_trace/) for a complete, runnable
override.

To recover Xapiand's exact behavior, the trace header maps the four macros back
to Xapiand's `log.h` (`L_SCHEDULER`/`L_CALL`/`L_DEBUG_HOOK`/`L_EXC`) and the nine
color names to `colors.h`.
