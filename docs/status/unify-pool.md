> Status: CURRENT (2026-11-02) workstream: one pool for the Kronuz libraries (developer).

# unify-pool (scheduler)

- Task: `ThreadedScheduler` runs on a `WorkPool` and one `WorkGroup`; `ThreadPool` is no longer used here.
- Branch: `phase7c/unify-pool`, from main `scheduler:2cbea95`; pins `threadpool:44551e5` (`phase7c/unify-pool`).
- Commit: `scheduler:c502949`. Review: none yet (the lead launches it).
- State: tests, mutants, sanitizers, A/A bench and CI done; waiting for the review.

## Change

- `ThreadedScheduler` owns `WorkPool pool` (`threads = num_threads`, `reserve = 0`, `name_format = format`) and `WorkGroup group`.
  `scheduler.h` includes `work_pool.hh`; `thread.hh` stays for `BaseScheduler`'s `Thread`.
- `size()`, `running_size()` read `group.pending()`, `group.running()`; `threadpool_size()` and `threadpool_capacity()` read `pool.threads()`
  (Xapiand's committer and fsyncher metrics use them).
- `finish(wait)`: `end(wait)`, set `stopped`, `group.cancel()`; `join(timeout)`: wheel thread, `group.cancel()`, then
  `group.on_idle` with a condition variable up to the deadline (false on timeout, `cancel_idle` or wait for a firing callback).
  `WorkGroup::wait()` is not used, so a finish from inside a task times out as before instead of aborting.
- The wheel dispatches with `group.enqueue([task] { (*task)(); })`: one box allocation per dispatched task (the old queue held the
  `shared_ptr` inline). A dispatch after `stopped` is skipped.
- Thread policy: unchanged and ignored at run time (`setup_thread` ignores it); workers are `regular`.

## Proof

- Tests: `ctest` (RelWithDebInfo, asserts live): scheduler, scheduler_no_memory pass. New cases `test_threaded_pool_shape`
  (3 workers all run, `running_size() == 3`, `size() == 2`, `finish(0)` drops the 2, `join(200ms)` false while a task runs, true after),
  `test_finish_drops_due_tasks`.
- Mutants (`.scratch/mut.sh`, each fails the test): finish keeps queued tasks (`sched.size() == 0`); dispatch after finish
  (`test_finish_drops_due_tasks`); join does not wait for the group; join never times out (`!sched.join(200ms)`);
  `running_size()` returns the backlog; `size()` returns the load; the pool keeps a reserve (`g_hold_started == 3`).
- Bench, i9 Mac, `quiet strict`, 6 cycles x 3 binaries interleaved (base, new, and base again as the A/A control), medians of 5
  (`add`, `fire`) and 7 (`log`) rounds per process, raw in `docs/status/unify-pool/ab.txt`, `bench/bench_pool.cc`, 4 workers, 200,000 tasks:

  | row | base `2cbea95` | A/A control | new |
  |---|---:|---:|---:|
  | `add()` 1 producer, ns | 196.6 (194 to 201) | 204.4 (196 to 213) | 205.4 (200 to 230) |
  | fire 200,000 tasks due at once, ns/task | 5681 (5624 to 5909) | 5696 (5571 to 5758) | 480 (465 to 516) |
  | logger `L_INFO`, 1 producer, ns | 671.7 (649 to 715) | 689.2 (634 to 719) | 672.6 (660 to 703) |

  `add()` and `L_INFO` do not touch the pool: new is within the control's spread. Firing a backlog through the pool is 11.8x faster
  (the old pool woke a sleeper per task). `L_INFO` bench: `.scratch/logbench` (logger main with the scheduler overridden).

- Sanitizers: `preflight.sh` (mac hardened, mac ASan+UBSan, Linux and FreeBSD VMs) ok on both repos at the branch heads;
  TSan (Homebrew LLVM, `-O1`) clean on `scheduler_test` and on threadpool's `work_counts`, `work_group`, `work_pool`, `work_wait`,
  `work_scheduling`. `scheduler_no_memory` skips TSan by design.
- Findings on the way: CI's Linux ASan run caught the no_memory test's operator new replacement missing the nothrow forms
  (the pool's box uses `new (std::nothrow)`): fixed in `test/no_memory.cc`. CI's FreeBSD run caught a timing flake in
  threadpool `work_group.cc` case 4 (a flush ended before the query looked; not caused by this work): the test now holds the flushes.
- CI (workflow_dispatch on the branch): threadpool run 41518748791 all jobs green; scheduler run 41518757480 all jobs green (macos-arm64 ran after a runner-capacity cancel).

## Next

After the review: threadpool steps 3 to 5 (delete the `ThreadPool` family, repin logger, strata, prospect and the other
consumers, then Xapiand on `deps/pins-2026-11`, the Detent note). Land threadpool and scheduler on main when the review clears.

## Blocked on

Nothing.
