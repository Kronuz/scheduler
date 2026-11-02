# Changelog

All notable changes to `scheduler` are recorded here, newest first. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## 2026-11-01

### Changed

- `ScheduledTask` is 72 bytes on a 64-bit target, from 96 (it was 48 before `post()`). Every task a
  caller makes paid for the fields of `post()`, posted or not, and `make only` (`std::make_shared`
  of a trivial task and a clock reading) was the one row slower than the commit before `post()`
  (`ae3f0f9`). The stack link and the owning reference a task holds while it waits on `post()`'s
  stack, and the filing number and tick it gets once the worker has filed it, never coexist, so
  they share one 24-byte union (`hand`, with `begin_posted()` and `end_posted()` for the stages),
  and `post_ordered` sits in the padding after `atom_cancelled`. libc++ puts 24 bytes of control
  block in front of the task, so the allocation is 96 bytes where it was 128 (and 80 at `ae3f0f9`).
  `make only`, ns per task, 15 interleaved runs per binary in one `quiet strict` hold per machine
  (START `ae3f0f9`, before `80d640f`, after; the ratio is START over the row, the A/A is START
  against START again):

  | | START | before | after | START / before | START / after | A/A |
  |---|---:|---:|---:|---:|---:|---:|
  | Apple M4 Pro, 1 producer | 36.2 | 37.1 | 36.9 | 0.986 | 0.987 | 1.008 |
  | Apple M4 Pro, 4 producers | 41.3 | 44.4 | 43.4 | 0.958 | 0.957 | 1.007 |
  | Intel i9-9980HK, 1 producer | 137.9 | 144.2 | 133.7 | 0.965 | 1.015 | 1.034 |
  | Intel i9-9980HK, 4 producers | 142.8 | 148.4 | 141.6 | 0.951 | 1.017 | 0.999 |

  The i9 is back to START. The M4 Pro with four producers stays 4% above it: the task would have to
  be 56 bytes to land in START's 80-byte allocation class, which needs the hand-off fields out of the
  task (scheduler#2). The other rows of `scheduler_bench_post` (add, post, post_ordered, one and
  four producers), before against after with an A/A: -1.2% to +12% on the M4 Pro, -1.1% to +12% on
  the i9, none slower beyond the A/A spread.
- `test/task_size.cc` (`scheduler_task_size`): the task is 72 bytes, the posted stage holds the task
  and gives the hold up once, and the filed stage reads zero for a task never posted and after a
  posted stage ends. With the old layout the size check fails (96, not 72). With `end_posted()` not
  zeroing the filed stage the stage test fails, and with `begin_posted()` not taking the hold
  `scheduler_post` crashes.

### Tests

- `test/no_memory.cc` covers an allocation failure in the worker's arrival queue
  (`arrivals.push_back`, review LPH-R6), which the `post_ordered()` path files into. It
  measures how a `std::deque` of the queue's element type takes a new block (libc++ at the
  first `push_back`, libstdc++ at the 32nd), posts that many ordered tasks, and refuses the
  block the worker asks for when it files the last one. A due task runs, one due 500 ms
  later is dropped and released, and a task posted afterwards runs, so the worker survives.
  Without the handler around the `push_back`, the exception ended the worker's thread and
  the due task never ran: the test fails on that header.

## 2026-10-31 (audit fixes)

### Fixed

- `add()` reads the published wakeup time, and decides whether to signal, only after its
  seq_cst fence (audit S1). It read the time before the fence, and the wheel insert is
  release-only, so the C++ model allowed it to read a target between now and the task, skip
  the signal, and leave a worker that had raised its target and missed the insert asleep
  until that target (up to 30 s). No run on arm64 or x86 shows it (the insert would have to
  stay in a store buffer for a whole worker pass); `scheduler_handshake_model` reads the
  order from `scheduler.h` and finds the loss in 88 of 7 746 store-buffer interleavings of
  the old order and in none of the new.

### Changed

- A worker pass that finds a task already due walks at once, without taking the mutex and
  without setting `sleeping` (audit S2). Every pass took the mutex and announced `sleeping`
  around its second look at the wheel, so a producer adding tasks while the worker ran them
  found it "sleeping" with a past target and queued on the mutex to signal it. One
  producer's `add()` of tasks due now, Apple M4 Pro, `quiet strict`, median of 7, two
  rounds (a probe from the audit, pre-made tasks): 411 / 401 ns before the lost-task fix,
  503 / 508 after it, 523 / 526 on the previous main, 390 / 424 now. Four producers:
  920 / 884 ns before, 839 / 849 now. `post()` and `post_ordered()` with one producer stay
  at 20 to 22 ns. `busy_pass` (`test/post.cc`) fails if a pass with a task due takes the
  sleep path.
- The stash pin moves to 78a18f3, whose leaf inserts resume their chain walk at the furthest
  node claimed instead of the leaf's head (LPH-R10). Every task due in one 1 ms slot, and
  every overdue task (filed at the wheel's floor), shares one leaf, so a burst or a backlog
  cost the inserting thread quadratic time: a TSan build of `producers` (`test/post.cc`, four
  threads posting 200 000 tasks due within 2 ms) timed out at 30 s in 3 of 5 runs on a loaded
  Linux VM and now runs in 4.1 to 4.8 s (10 of 10); the test is back to that shape. One
  producer's `add()` of tasks due now: 390 / 424 ns before, 224 / 252 ns now (Apple M4 Pro,
  as above); due in 1 ms: 319 / 321 ns before, 149 / 149 now; four producers, due now:
  839 / 849 ns before, 628 / 626 now.
- ctest runs the soak with three producers too (`scheduler_soak_producers`).
- A worker pass files the posted stack once when it finds work due (it filed a second time just
  before walking), and the worker no longer writes `ending` on every pass while no `finish()` has
  begun (it decremented it and stored -1 back; a `finish()` landing between the two was lost). With
  the worker filing at stash 78a18f3's speed, both writes hit lines every producer touches on each
  call: four producers' `post()` of tasks due now cost 531 to 649 ns per call (median 614, 15 runs)
  against 432 to 615 (median 499, 17 runs) with the parent stash, and costs 462 to 595 (median 534,
  10 runs) now (Apple M4 Pro, 100 000 to 200 000 tasks per producer, median of 7 per run,
  alternated). On the Intel i9 laptop the parent stash does not finish that row: the worker, filing
  400 000 overdue tasks into one leaf, has not run them 30 s later.

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
