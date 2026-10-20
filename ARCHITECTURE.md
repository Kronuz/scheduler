# Architecture

The internal design of `scheduler`: the timer wheel, the scheduler thread loop,
the two run strategies, and the debouncer's timing model. For usage see
`README.md`; for the repo map and invariants see `AGENTS.md`.

## The stack

Three layers, each in one header, stacked strictly:

```
  stash.h        a lock-free hierarchical slot store (a timer-wheel primitive; a separate library)
      ▲
  scheduler.h    a 24-hour multi-resolution timer wheel + a thread that runs due tasks
      ▲
  debouncer.h    per-key throttle / debounce on top of the scheduler
```

`stash` and the thread/pool primitives are their own libraries, pulled in by
`FetchContent`. This repo is the top two layers plus the small pieces the
debouncer needs (`callable_traits.hh`); `random_time` comes from the
[random](https://github.com/Kronuz/random) library.

## Layer 0 (dependency): the slot store

The wheel is built out of [`stash`](https://github.com/Kronuz/stash)'s
`StashSlots` and `StashValues`. `StashSlots<T, Size, Div, Mod>`
is a keyed level: a key maps to slot `(key / Div) % Mod`, and `T` is itself a
nested level (or, at the bottom, a `StashValues` leaf — an append-only list with
separate walk and clean cursors). Levels nest to give multiple resolutions.
`next()` walks a key window in one of three modes — walk (consume), peep
(look-ahead, no mutation), clean (GC emptied slots). All inserts use atomic
compare-exchange, so concurrent producers never lock. See the `stash` repo's
`ARCHITECTURE.md` for the full story.

## Layer 1: the timer wheel and its thread

### `SchedulerQueue` — the 24-hour wheel

`SchedulerQueue` nests four `StashSlots` levels over a `StashValues` leaf, keyed
on nanoseconds since the steady-clock epoch:

```
  4800 × 18 s  →  36 × 500 ms  →  10 × 50 ms  →  50 × 1 ms  →  task list
       │              │              │              │
       │              │              │              └─ Div = 1 ms,    Mod = 50
       │              │              └──────────────── Div = 50 ms,   Mod = 10
       │              └─────────────────────────────── Div = 500 ms,  Mod = 36
       └────────────────────────────────────────────── Div = 18 s,    Mod = 4800
```

`4800 × 18 s = 86 400 s = exactly 24 h`. The bottom level has 1 ms granularity;
the wheel as a whole spans 24 hours. A task scheduled further out than that
overflows: `StashSlots::add` returns false and stores nothing, and
`SchedulerQueue::add` calls the trace hooks (no-ops by default) and returns
`schedule_result::past_horizon`. Every `add` is `noexcept` and returns a
`schedule_result`: `scheduled`, or why the task will never fire: `past_horizon`,
`no_memory` (it catches stash's `std::bad_alloc`) or, in `BaseScheduler`,
`stopping` once `finish()` began. A caller that ignores the result drops the task
as before. Within the horizon, the task goes into the slot for its wakeup-time
key. The queue's `walk`, `peep`, `clean` and `clean_checkpoint` are `noexcept`:
stash's walk never allocates, and the `shared_ptr` task values copy and test
without throwing.

The queue keeps two `StashContext`s: `ctx` for peep/walk (consume), `cctx` for
clean (GC). `clean_checkpoint()` snapshots the first/last valid keys from `ctx`
into `cctx` before each clean so the GC chases the consumer without racing it.
`clean()` reclaims up to the walk's low-water mark (`ctx`'s first valid key), with
no wall-clock margin: stash's sentinel frontier and pending floor make that a safe
boundary, so a task that is overdue but not yet run is never reclaimed.

### `ScheduledTask` — the cancellable unit

A CRTP, `enable_shared_from_this` task carrying a `wakeup_time` and two atomic
timestamps (`atom_created_at`, `atom_cleared_at`). `operator bool()` reports "not
yet cleared." `clear()` cancels with a single `compare_exchange_strong` on
`atom_cleared_at`: the first caller wins and returns true, every later caller
loses. This one-shot atomic cancel is what makes a scheduled callback cancellable,
and the debouncer's per-key collapse is built entirely on clearing the previous
task before scheduling a new one.

`operator()()` dispatches through the CRTP to the concrete task `Impl`.

### `BaseScheduler` — the thread loop

`BaseScheduler` is a `Thread` (from the `threadpool` dep) parameterized on the
concrete scheduler `Impl` (CRTP) so it can call back into the right run strategy.
Its loop:

1. Propose a wakeup time: `now + 30s` when idle, `now + 100ms` when tasks pend.
2. `peep` the queue for anything due sooner; if found, lower the proposed wakeup
   to that task's `wakeup_time` and flag `pending`.
3. CAS the proposed time into `atom_next_wakeup_time`.
4. Under `mtx`, reload `atom_next_wakeup_time` and `wait_until` on the condvar —
   so the thread sleeps until the earliest task is due, or until an `add()`
   notifies it sooner.
5. On wake, `clean_checkpoint()`, then `walk()` every due task and hand each to
   the run strategy (`static_cast<SchedulerImpl*>(this)->operator()(task)`), then
   `clean()`.

`add(task)` lowers `atom_next_wakeup_time` toward the new task's wakeup time and
notifies the condvar, so a task scheduled sooner than the current sleep wakes the
loop immediately. The mutex is held across the load-then-wait in the loop so an
`add()` can't slip in between and be lost. One mutex, one condvar, a lock-free
queue underneath.

`end(wait)` sets a countdown that lets the loop drain pending work, then exit.

### Two run strategies

`Scheduler<Impl, policy>` runs each due task **inline** on the scheduler thread:
it checks the task isn't cleared, `clear()`s it (so it runs at most once), and
calls `task->operator()()` directly. This is the right shape for short, reliable
callbacks — Xapiand's logger thread uses it.

`ThreadedScheduler<Impl, policy>` owns a `ThreadPool` and **dispatches** each due
task to a worker (`thread_pool.enqueue(task)`), so a slow task can't stall the
wheel. Its `finish()`/`join()` shut down the loop and the pool together, splitting
the join timeout across the workers. The debouncer is built on this.

Both derive from `BaseScheduler` and differ only in `operator()(TaskType&)`.

## Layer 2: the debouncer

`Debouncer<Key, Func, Tuple, policy>` extends `ThreadedScheduler<DebouncerTask>`
and keeps a per-key status map (`unordered_map<Key, Status>` under a mutex). Each
`Status` holds the key's one pending `DebouncerTask`, its `max_wakeup_time`
ceiling, the `target` time the call is due, and the latest arguments.

### The timing model

`debounce(key, args...)` (and `delayed_debounce(delay, ...)`) decide a
`next_wakeup_time` for the key:

- A **fresh** key fires after `debounce_timeout`. Its `max_wakeup_time` ceiling is
  set to `now + random(min_force, max_force)` — the randomized **force window**.
- A **subsequent** touch pushes the call out by `debounce_busy_timeout`, but the
  result is clamped to never exceed `max_wakeup_time`. So a key that is touched
  forever still fires by the force window. The randomization spreads a thundering
  herd of simultaneously-due keys across time instead of firing them all at once.
- If a call is already pending for the key, the touch re-arms it in place: it
  moves `target` to the later of the two times and replaces the arguments, under
  the status mutex, and adds nothing to the wheel. A touch never fires a key
  earlier than it was already due, and the call fires with the latest arguments.

When the task fires (`DebouncerTask::operator()`), it takes the status mutex. If
`target` moved past the task's own `wakeup_time` (a touch re-armed it), it adds a
fresh task at `target` and returns. Otherwise it takes the arguments and, in the
same critical section, releases or throttles the key, then runs
`std::apply(func, args)` outside the lock. Doing both under one lock is what keeps
a touch from landing between them and being dropped.

Refusals. `debounce` returns the scheduler's `schedule_result` and is `noexcept`
(`std::bad_alloc` becomes `no_memory`). A touch that adds a task does so under the
status mutex, so a later touch that finds the task pending knows it is on the
wheel; if the add is refused, the key's status is erased and nothing stays
pending. A touch on a pending call returns `scheduled` without adding; if copying
its arguments fails it returns `no_memory` and the pending call keeps its earlier
arguments. A re-arm refused in the fire path has no caller to tell, so the task
runs the call there and then, a little early, and releases or throttles the key
as a fire does; a refused throttler just releases it. Releasing or throttling:

- If `throttle_time <= debounce_timeout`, just `release(key)` (erase the status),
  so the next touch starts fresh.
- Otherwise it schedules a **throttler** task `now + throttle_time` out and marks
  the status as a throttler (`max_wakeup_time == time_point::min()`). Until that
  throttler fires and `release`s the key, new touches are held back to at least
  the throttler's wakeup time — a post-fire floor on the key's firing rate.

A touch never cancels a pending call, so the wheel holds at most one entry per key
plus one per re-arm, instead of one per touch: cancelled tasks used to stay in the
wheel with their arguments until their slot drained (`bench/bench_churn.cc`;
`docs/perf/lazy-rearm.md`). Only a throttler is still cancelled, when a touch turns
a throttled key back into a pending call; the throttler releases a key only while
it is still that key's task.

### Deducing the argument tuple

`make_debouncer<Key>(...)` (and the unique/shared variants) take the callback and
deduce its argument tuple via `callable_traits<decltype(func)>::arguments_type`,
so the `Tuple` template parameter is inferred rather than spelled out.
`callable_traits.hh` is a standalone callable-introspection header (return type,
arity, argument types as a tuple) copied verbatim from Xapiand; it has no
dependencies of its own.

## The random helpers

The force window draws from `random_time(min, max)`, a uniform draw over a
`mt19937_64` seeded from `std::random_device`, kept `thread_local`. `random.cc`
carries it plus `random_int` and `random_real` (the three helpers the debouncer
pulls in). Xapiand's `random_string` was left behind because it dragged in
unrelated Xapiand code.

## Tracing seam

`scheduler.h` and `debouncer.h` are heavily instrumented, but every trace line
flows through four macros — `L_SCHEDULER`, `L_DEBUG_HOOK`, `L_EXC`, `L_CALL` —
that are no-ops by default (`scheduler_trace.h`). The default build therefore has
no logging dependency and no tracing cost.

A subtle but deliberate consequence: `scheduler.h` builds its trace strings by
concatenating color identifiers (`"BaseScheduler::" + DODGER_BLUE + "WAKEUP" +
CLEAR_COLOR`), and those nine color names appear **only** inside the trace-macro
arguments. Because the default macros expand to nothing, the preprocessor
discards the arguments and the color identifiers are never compiled — so the
standalone build needs no color stubs at all. Only a trace header that turns the
macros into real string-building calls (like `examples/colored_trace/trace.h`)
has to define those nine identifiers. This was verified empirically: the library
and its test compile clean with `scheduler_trace.h` defining nothing for the
colors.

A consumer restores real, colored tracing by pointing `SCHEDULER_TRACE_HEADER` at
a header that defines the four macros and the nine colors — Xapiand maps them
back to `log.h` and `colors.h`. The `examples/colored_trace/` example is a
self-contained demonstration of the whole seam.

## Why this shape

The engine is unusually clean to lift out because the layering is strict and the
coupling is shallow: `stash` knows nothing of the scheduler, the scheduler knows
nothing of the debouncer, and the only cross-cutting concern (logging) is a set
of injectable macros. Each layer is independently useful — a lock-free slot
store, a cancellable O(1)-ish timer wheel that scales to many short-horizon
timers, and a randomized force-window debouncer — which is why they ship as a
stack of small libraries rather than one monolith.
