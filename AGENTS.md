# AGENTS.md

Working notes for agents modifying this repository. For the design read
`ARCHITECTURE.md`; for usage read `README.md`. This file covers the repo layout,
how to build and test, the invariants you must not break, and the traps that are
easy to fall into.

## Repo map

```
scheduler.h                 The timer wheel: ScheduledTask, SchedulerQueue, BaseScheduler, Scheduler, ThreadedScheduler. Header.
debouncer.h                 Debouncer, DebouncerTask, make_(unique_/shared_)debouncer factories. Header.
callable_traits.hh          Callable introspection (return/args/arity). Copied verbatim from Xapiand; zero deps. Header.
scheduler_trace.h           No-op L_NOTHING / L_DEBUG_HOOK / L_EXC / L_CALL hooks (stand in for Xapiand's log.h/colors.h).
test/test.cc                Tests: inline Scheduler order, clear() cancel, ThreadedScheduler, Debouncer collapse and force window.
examples/colored_trace/     Runnable demo that injects a real trace header to restore colored, formatted trace output.
CMakeLists.txt              STATIC library `scheduler` (+ alias scheduler::scheduler); FetchContents stash + threadpool; CTest test `scheduler`.
LICENSE                     MIT, Copyright (c) 2015-2019 Dubalu LLC.
README.md                   What it is, install, quick start.
docs/API.md                 The API reference and the tracing hooks.
docs/scheduler.d2           The README diagram's source (docs/scheduler.svg is rendered from it).
examples/demo.cc            A runnable tour (target scheduler_demo).
ARCHITECTURE.md             Internal design, the timing model, trade-offs.
```

The library is header-only. Its three dependencies — [`stash`](https://github.com/Kronuz/stash)
(the slot store), [`threadpool`](https://github.com/Kronuz/threadpool) (Thread +
ThreadPool + ThreadPolicyType), and [`random`](https://github.com/Kronuz/random)
(`random_time`) — are pulled in by `FetchContent` at their `main` tips and provide
`stash.h`, `thread.hh`, `threadpool.hh`, and `random.hh` on the include path.

## Build and run the test

```sh
cmake -B build && cmake --build build && ctest --test-dir build
```

The first `cmake -B build` clones `stash` and `threadpool` from GitHub at
their `main` tips. Expected output ends with
`all scheduler tests passed`, exit 0. The CMake `scheduler` target is a `STATIC`
library that requests `cxx_std_20`, links `stash` and `threadpool::threadpool`,
and exposes the source dir as a `PUBLIC` include. The test target is
`scheduler_test`; the registered CTest name is `scheduler`.

## Conventions

- **C++20.** Required (the threadpool dep uses `std::format` for worker naming,
  and the example uses `std::format`). Don't drop below it.
- **No Xapiand headers.** The only includes are the C++ standard library, the two
  FetchContent'd deps (`stash.h`, `thread.hh`, `threadpool.hh`), and the bundled
  headers. Do not add `log.h`, `colors.h`, `strings.hh`, etc. back; routing
  through the trace hooks is exactly what replaced them.
- **Tracing flows through four injectable macros**, all no-ops by default:
  `L_SCHEDULER`, `L_DEBUG_HOOK`, `L_EXC`, `L_CALL`. Defaults live in
  `scheduler_trace.h`, each `#ifndef`-guarded. `scheduler.h` reaches them through
  `#ifdef SCHEDULER_TRACE_HEADER` (include the consumer's header) `#else`
  (include `scheduler_trace.h`). `debouncer.h` gets them transitively via
  `scheduler.h`. A consumer injects its own via
  `-DSCHEDULER_TRACE_HEADER='"my_trace.h"'`, or defines the macros first. Keep any
  new trace call behind these macros; never assume they do anything.
- Tabs for indentation, double quotes in code, no em dashes in prose.

## Load-bearing invariants

- **`ThreadPolicyType` and the policy template parameter must stay.** They come
  from the `threadpool` dep and thread through `ScheduledTask`, `BaseScheduler`,
  `Scheduler`, `ThreadedScheduler`, `Debouncer`, and the factories. The policy is
  runtime-ignored today (see the threadpool repo), but it is kept for
  source-compatibility with Xapiand's many call sites. Deleting it breaks every
  one of them.
- **The color identifiers are referenced only inside trace-macro arguments.**
  `scheduler.h` names `BROWN`, `CLEAR_COLOR`, `LIGHT_SKY_BLUE`, `DIM_GREY`,
  `PURPLE`, `STEEL_BLUE`, `DODGER_BLUE`, `LIGHT_GREEN`, `FOREST_GREEN` only as
  arguments to `L_SCHEDULER(...)` / `L_DEBUG_HOOK(...)`. With the default no-op
  macros the preprocessor discards those arguments, so the identifiers are never
  compiled and **no color stubs are needed**. Verified empirically: the library
  and test build clean with `scheduler_trace.h` defining nothing for them. If you
  add a trace site that uses a color outside a no-op-able macro, you'd have to
  define the identifier — don't; keep colors inside the macros.
- **`SchedulerQueue`'s wheel geometry is exactly 24 h at 1 ms granularity.** The
  four `StashSlots` levels (`50×1ms`, `10×50ms`, `36×500ms`, `4800×18s`) multiply
  to 86 400 s. Changing a `Div`/`Mod`/`Size` changes the horizon or resolution
  and the overflow point. `SchedulerQueue::add()` maps stash's `stash_result` one
  to one (`placed`, `past_horizon`, `no_memory`); stash's `add` is `noexcept` and
  terminates on a failure after claiming a slot. `debounce` is `noexcept` too and
  catches `std::bad_alloc` from its own allocations as `no_memory`.
- **A refused debounce leaves nothing pending.** A touch adds its task under the
  status mutex and erases the key's status on any refusal. A fire-path re-arm
  refused as `stopping` or `no_memory` runs the call on the fire thread; one
  refused as `past_horizon` drops it (never run a day early). Either way the key
  is released. No `Status` may keep a task that is not on the wheel.
- **`ScheduledTask::clear()` is a single CAS on `atom_cleared_at`.** First caller
  wins (returns true), the rest lose. The whole cancellation story (and the
  debouncer's per-key collapse) depends on this being atomic and one-shot. Don't
  turn it into a plain store.
- **The scheduler loop's wakeup math is subtle.** `BaseScheduler::operator()`
  proposes a wakeup, `peep`s for something sooner, CASes `atom_next_wakeup_time`,
  and sleeps on a condvar under `mtx`. `add()` lowers `atom_next_wakeup_time` and
  notifies. The mutex must be held across the load-then-wait so an `add()` can't
  slip between them. Keep the lock discipline exactly as is.

## How to extend

- **Add a scheduler state query.** `ThreadedScheduler` forwards `size()` etc. to
  its pool; follow that pattern.
- **Plug in tracing / coloring.** Inject a trace header with
  `-DSCHEDULER_TRACE_HEADER='"my_trace.h"'` that defines the four macros (and, if
  it renders the messages, the nine color identifiers), or define them before
  including the headers. To recover Xapiand's behavior, map the macros to
  `log.h`'s and the colors to `colors.h`. See `examples/colored_trace/`.
- **Always extend the smoke test.** `test/test.cc` is the only executable check.
  Any behavioral change should grow a corresponding assertion there. Keep timing
  windows generous; never assert a tight race.

## Traps

- **Don't delete `ThreadPolicyType`** thinking it is dead. It is unused at runtime
  but load-bearing at compile time for the consumer's call sites, and it comes
  from the `threadpool` dep, not this repo.
- **Don't add color stubs to `scheduler_trace.h` for the default build.** They are
  not needed — the no-op macros discard the color arguments before compilation.
  Adding them is dead code that suggests the wrong mental model.
- **Don't pass non-formattable types to `std::format` in a trace header.**
  `scheduler.h` hands raw `steady_clock::time_point`s into a couple of trace
  lines (the `ADDED` / `ADDED_NOTIFY` calls). libc++'s `std::format` cannot
  render those, and it is a *compile-time* error, not catchable at runtime. The
  example's `trace.h` stringifies arguments itself to sidestep this; a real
  logger (Xapiand's) has a formatter that handles its own types.
- **`random_time` comes from the random library.** Don't vendor it back: a binary
  that links both would carry two definitions. CMake reuses a consumer's
  `random::random` target when one exists, so declare random before scheduler if you
  name its FetchContent differently.
- **The deps track `main`.** `CMakeLists.txt` fetches `stash` and `threadpool` at
  their tips, so a change there reaches this build (the nightly CI run catches
  drift). The wheel relies on `stash`'s `StashSlots`/`StashValues` API and the scheduler
  on `threadpool`'s `Thread`/`ThreadPool`/`ThreadPolicyType`.

## Standalone vs. Xapiand

This is a standalone extraction from
[Xapiand](https://github.com/Kronuz/Xapiand). The delta from the original is pure
decoupling:

- `scheduler.h` and `debouncer.h` dropped `#include "log.h"`. The four trace
  macros (`L_SCHEDULER`, `L_DEBUG_HOOK`, `L_EXC`, `L_CALL`) now resolve through
  `scheduler_trace.h` (no-op by default). The `stash.h`, `thread.hh`,
  `threadpool.hh` includes are unchanged but now resolve to the FetchContent'd
  standalone libraries.
- The uniform-random helpers come from the
  [random](https://github.com/Kronuz/random) library, as in Xapiand.
- `callable_traits.hh` was copied verbatim — it had zero Xapiand dependencies
  already.

Since the extraction the logic has diverged in two places: the queue's `clean` is
walk-based (stash's sentinel frontier), and a debouncer touch re-arms the pending
call in place (the latest arguments win, `e7954f2`; no cancel and re-add, see
`docs/perf/lazy-rearm.md`). Xapiand now fetches this
library at `main`, so it runs the same code. Keep extraction hygiene separate from behavior
changes so they can be reconciled with upstream. To restore Xapiand's tracing and
coloring without editing these files, see "Tracing and coloring" in
[docs/API.md](docs/API.md).
