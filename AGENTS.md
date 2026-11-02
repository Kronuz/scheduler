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
test/test.cc                Tests: inline Scheduler order, clear() cancel, ThreadedScheduler, Debouncer collapse and force window, the add-between-peep-and-sleep race (via SCHEDULER_TEST_HOOK).
test/soak.cc                Lost-task soak (`scheduler_soak`): any task that never fires fails it.
test/post.cc                post() and post_ordered(): order (one thread, across threads, mixed delays against the wheel's oracle), cancel, wake, horizon, many producers, finish() racing the posters.
test/handshake_model.cc     add()'s side of the sleep handshake, read from scheduler.h, against a store-buffer model of the worker's pass.
test/task_size.cc           ScheduledTask is 56 bytes, and the posted and filed stages of its hand-off fields share storage (`scheduler_task_size`).
test/no_memory.cc           Allocation failure (`scheduler_no_memory`): add() and debounce() report it; a refused block in the worker's arrival queue runs a due task, drops the others and leaves the worker running. Not built under TSan (it replaces operator new).
bench/bench_post.cc         What a producer pays to arm a task: add(), post(), post_ordered() (`SCHEDULER_BENCH`).
examples/colored_trace/     Runnable demo that injects a real trace header to restore colored, formatted trace output.
CMakeLists.txt              STATIC library `scheduler` (+ alias scheduler::scheduler); FetchContents stash + threadpool; CTest test `scheduler`.
LICENSE                     MIT, Copyright (c) 2015-2019 Dubalu LLC.
README.md                   What it is, install, quick start.
docs/API.md                 The API reference and the tracing hooks.
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
  slip between them. Keep the lock discipline exactly as is. `add()` reads the
  published wakeup time and `sleeping` only after its seq_cst fence
  (`scheduler_handshake_model` reads the order from this header and fails
  otherwise). A pass that finds a task due already walks at once, without the mutex
  and without setting `sleeping`; `busy_pass` (`test/post.cc`) fails if it sleeps
  instead, which made one producer's `add()` cost a third more. A busy pass files the
  stack once, and no pass writes `ending` before `finish()` begins: producers touch
  `posted` and read `ending` on every call, so each extra worker write there costs
  four producers' `post()` (CHANGELOG.md).

- **`post()` hands a task over; `ingest()` files it.** Posters push onto `posted`, a lock-free
  stack, with one compare-exchange (the task carries the link, `hand.posted.next`, and an owning
  `hand.posted.hold`, so nothing is allocated); the worker takes the whole stack with one exchange, turns it around and
  files the tasks oldest first: a `post_ordered()` task into the `arrivals` queue, any other into
  the wheel. The worker files at the top of each pass, under the mutex after it stores `sleeping`
  (the Dekker pair: poster pushes then loads `sleeping`, worker stores `sleeping` then loads
  `posted`, all seq_cst), and after it wakes, before it fires. A task is posted once and never also
  added; `hand` and `post_ordered` belong to the hand-off.
- **A task is 56 bytes, and every task a caller makes pays for them.** libc++ puts 24 bytes of control
  block in front of what `make_shared` builds, so 56 bytes is an 80-byte allocation: the size class
  every task had before `post()` existed (48 bytes), and one more field is the next class up and
  `make only` slows by 4% with four producers on the M4 Pro (`scheduler_bench_post`; CHANGELOG.md). The posted stage of `hand`
  (the link and the hold) and its filed stage (`seq`, `tick`: the filing number and tick that merge
  the arrival queue with the wheel) are one 24-byte union, because they never overlap: the worker ends the
  posted stage (`end_posted()`, which returns the hold and leaves the filed stage zero) before it
  writes the filed one, and a task never posted reads zero there, which `before()` takes for "never
  filed". Keep `post_ordered` in the padding after `atom_cancelled`. `scheduler_task_size` checks the
  size and the stages.
- **A task does not derive `std::enable_shared_from_this`.** The base is 16 bytes in every task and
  `make_shared` increments the control block's weak count to fill it, an atomic read-modify-write that
  `make only` pays on every task (12 to 14% of the row on the M4 Pro). Nothing in the library or in
  logger calls `shared_from_this()` on a task; a task that needs its owner is handed the pointer.
- **`finish()` closes the stack on every exit of the worker loop.** When the worker finds nothing
  due and `finish()` has begun it exchanges a closed mark into `posted` and files what it held;
  when the countdown reaches zero (or `finish(0)`) it does the same after the loop and runs what
  is due. A `post()` that pushes after that sees the mark and returns `stopping`; one that
  pushed before is filed. Without the close a poster that passed its `stopping` check just
  before `finish()` could push after the worker's last look, and the task, which holds itself
  through `hand.posted.hold`, would never run and never be freed. Only the worker ever writes the mark.
  `finish_exit_none`, `finish_exit_countdown` and `finish_exit_zero` (`test/post.cc`) are the check for each exit (`test_finish_race` alone passes without the
  close).
- **No task code runs under `mtx`.** Filing a posted task can destroy a cleared one or run a
  refused one, and a task that posts takes `mtx` to signal. The worker therefore only tests
  `has_posted()` under the lock (the Dekker half) and files after it unlocks.
- **The arrival queue is merged, never fired alone.** `fire_due()` takes the wheel's next due task
  and the queue's head and runs the one that comes first by (tick, filing order), the order the
  wheel alone would give. The tick is the one the task is *filed* in: `ingest()` records it with
  `SchedulerQueue::filing_tick()`, which applies the floor stash files an overdue key at. Comparing
  due times instead breaks the order whenever the worker was busy while tasks were posted
  (`mixed_order(true)` in `test/post.cc` fails without it). `limit` is read before the wheel is walked, so a walk that finds nothing
  means no wheel task is due before `limit`, and only then may queue tasks run without looking at
  the wheel. A `post_ordered()` poster must post nondecreasing wakeup times (a wakeup of its clock reading
  plus a fixed delay is fine). Across posters the times may fall out of order by the gap between
  a clock reading and the push: a task behind a later head waits for it and wheel tasks due in
  between pass it, but posts ordered by anything that orders them keep due order
  (`test_ordered_posters`).
  Tasks added with `add()` have no filing order, so they and `post_ordered()` tasks are not
  ordered against each other within a tick.

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
  windows generous; never assert a tight race by luck: pin a race with
  `SCHEDULER_TEST_HOOK` (as `test_add_between_peep_and_sleep` does) and soak it with
  `scheduler_soak`. ctest runs the soak with one producer and with three
  (`scheduler_soak_producers`). A TSan from LLVM 12 or older reports races inside
  stash's insert with two or more producers that are not there: it ignores a failed
  compare-exchange's acquire (stash's AGENTS.md, Traps); use a newer one.

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
[Xapiand](https://github.com/Kronuz/Xapiand). It differs from the original in
these places:

- `scheduler.h` and `debouncer.h` do not include `log.h`. The four trace macros
  (`L_SCHEDULER`, `L_DEBUG_HOOK`, `L_EXC`, `L_CALL`) resolve through
  `scheduler_trace.h` (no-op by default). The `stash.h`, `thread.hh`, and
  `threadpool.hh` includes resolve to the FetchContent'd standalone libraries.
- The uniform-random helpers come from the
  [random](https://github.com/Kronuz/random) library, as in Xapiand.
- `callable_traits.hh` is a verbatim copy; it has no Xapiand dependencies.
- The queue's `clean` is walk-based (stash's sentinel frontier).
- A debouncer touch re-arms the pending call in place, and the latest arguments
  win (no cancel and re-add, see `docs/perf/lazy-rearm.md`).

Xapiand fetches this library at `main`, so it runs the same code. Keep
extraction hygiene separate from behavior changes so they can be reconciled with
upstream. To restore Xapiand's tracing and coloring without editing these files,
see "Tracing and coloring" in [docs/API.md](docs/API.md).
