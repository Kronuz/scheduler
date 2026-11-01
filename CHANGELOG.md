# Changelog

All notable changes to `scheduler` are recorded here, newest first. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## 2026-10-31 (producer hand-off)

### Added

- `post(task, wakeup_time, now)` and `post_ordered(task, wakeup_time, now)`: a producer hands a
  task to the scheduler thread with one compare-exchange onto a lock-free stack, instead of
  inserting it into the wheel on its own thread. The caller allocates nothing and reads no
  clock (`now` is its own reading); it signals the thread only when the thread sleeps past the
  task, and once per sleep however many tasks it posts. The scheduler thread files the tasks:
  `post()` ones into the wheel (so the wheel has one inserter and the order within a tick is the
  posting order), `post_ordered()` ones, for callers whose own wakeup times never decrease, into
  an arrival queue that it merges with the wheel by tick and filing order and never puts on the
  wheel. A task cleared before it is filed never enters the wheel. `finish()` closes the stack on every
  exit of the worker loop, so a task posted as it begins is refused, run or released, never left
  holding itself on the stack. On an Apple M1 Pro one producer
  pays 57 ns per task for `post()` and 56 ns for `post_ordered()` against 367 ns for `add()`
  (the task's own allocation is about 50 ns of each; `bench/bench_post.cc`). `add()` and the
  rest of the API are unchanged.
- `test/post.cc`: order from one thread and across threads, mixed delays against the wheel's
  due-order oracle, cancel, wake, horizon, four producers, and a `finish()` that races three
  posters over 200 rounds (2 million accepted tasks, all run, none twice).

## 2026-10-31

### Fixed

- A task added between the worker's last `peep` and its sleep could wait out the
  worker's whole 30 s idle target although it was due now, which looked like a
  lost task: about one in 60 000 adds in a 20 000-task burst soak (2 623 of
  140 M adds; first seen as about once per two million adds in a burst test).
  `add()` found the published wakeup stale and had nothing to lower, and the
  worker was not yet `sleeping`, so `add()` sent no signal. A Dekker pair closes
  the window: the worker `peep`s once more under the mutex before it waits. On a
  Linux x86_64 VM, 140 M burst adds and 120 M random-burst adds lose none (7 x
  20 M burst adds lost 2 623 before). No API or timing change otherwise.
  `test_add_between_peep_and_sleep` pins the interleaving through the new
  `SCHEDULER_TEST_HOOK` seam, and `scheduler_soak` soaks it.

### Changed

- `add()` skips the wake-up signal while the worker is awake. It used to take the
  mutex and signal the condition variable for every task whenever the recorded
  next wake time was stale, which is the whole time the worker runs tasks. The
  public API and task ordering are unchanged.

### Removed

- The burst half of the earlier wake-up test: it dropped a task about once per
  two million adds on the unmodified scheduler too, so it could not gate CI. A
  deterministic test (a task due in 30 ms fires on time while the worker sleeps
  toward a 5 s target) replaces it.

## 2026-10-28

### Changed

- A debouncer touch re-arms the pending call in place instead of cancelling it
  and adding a new task (lazy re-arm). On macOS x86_64 (i9-9980HK), at 100,000
  touches/s for 30 s with the committer's timeouts, against the previous main:

  | 100,000 touches/s, 30 s | before | after | change |
  |---|---|---|---|
  | 1,000 keys: tasks alive | 506,892-507,300 | 1,002 | 506x fewer |
  | 1,000 keys: heap | 99.1-107.9 MB | 1.3-1.6 MB | 98.5% less |
  | 1,000 keys: RSS | 238.6-253.0 MB | 4.9-5.1 MB | 98% less |
  | 1,000 keys: CPU per 42 s run | median 11.5 s | median 3.2 s | 3.6x less |
  | 100,000 keys: tasks alive | 282,192-283,121 | 92,269-92,385 | 3.1x fewer |
  | 100,000 keys: heap | 71.2-78.4 MB | 37.0-38.9 MB | 50% less |
  | 100,000 keys: CPU per 42 s run | median 19.5 s | median 21.4 s | about 10% more, ranges overlap |

  Calls fired are the same, and every key fires its last touch. Full method and
  results: [docs/perf/lazy-rearm.md](docs/perf/lazy-rearm.md).
- Every `add` overload and `debounce` / `delayed_debounce` are `noexcept` and
  return `schedule_result { scheduled, past_horizon, stopping, no_memory }`. A
  caller that ignores the result keeps the old drop-silently behavior.

### Fixed

- The throttler erased the key's status unconditionally, so a touch that turned
  the key back into a pending call between the throttler's dispatch and its run
  lost that call. It erases the status only while it is still the key's task.
