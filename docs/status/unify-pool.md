> Status: CURRENT (2026-11-02) workstream: one pool for the Kronuz libraries (developer).

# unify-pool (scheduler)

- Task: `ThreadedScheduler` runs on a `WorkPool` and one `WorkGroup`; `ThreadPool` is no longer used here.
- Branch: `phase7c/unify-pool`, merged with main `scheduler:2bb0a8f` (review CR1); pins the threadpool branch head (see `CMakeLists.txt`).
- Commit: `scheduler:c502949`. Review: `docs/reviews/unify-pool-code.md` (approve with conditions).
- State: landed on main (`scheduler:d94f287`). Step 4 on branch `phase7c/drop-threadpool`: pin threadpool at its drop-threadpool head (see below).

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
- Bench, i9 Mac, `quiet strict`, 6 cycles x 3 binaries interleaved (main, new, and main again as the A/A control), medians of 5
  (`add`, `fire`) and 7 (`log`) rounds per process, base = scheduler main `2bb0a8f`, 4 workers, 200,000 tasks, raw in
  `docs/status/unify-pool/ab-main.txt` (`bench/bench_pool.cc`; the first run against the old base `2cbea95` is `ab.txt`, superseded
  by review CR1):

  | row | main `2bb0a8f` | A/A control | new |
  |---|---:|---:|---:|
  | `add()` 1 producer, ns | 189.6 (179 to 205) | 183.8 (178 to 187) | 182.1 (178 to 200) |
  | fire 200,000 tasks due at once, ns/task | 2522 (2457 to 2640) | 2570 (2429 to 2599) | 443 (422 to 454) |
  | logger `L_INFO`, 1 producer, ns | 568.2 (557 to 588) | 551.1 (531 to 584) | 555.4 (545 to 665) |

  `add()` and `L_INFO` do not touch the pool: new is within the control's spread. Firing a backlog through the pool is 5.7x faster.
  `L_INFO` bench: `.scratch/logbench` (logger main with the scheduler overridden).
- Sanitizers: `preflight.sh` (mac hardened, mac ASan+UBSan, Linux and FreeBSD VMs) ok on both repos at the branch heads;
  TSan (Homebrew LLVM, `-O1`) clean on `scheduler_test` and on threadpool's `work_counts`, `work_group`, `work_pool`, `work_wait`,
  `work_scheduling`. `scheduler_no_memory` skips TSan by design.
- Findings on the way: CI's Linux ASan run caught the no_memory test's operator new replacement missing the nothrow forms
  (the pool's box uses `new (std::nothrow)`): fixed in `test/no_memory.cc`. CI's FreeBSD run caught a timing flake in
  threadpool `work_group.cc` case 4 (a flush ended before the query looked; not caused by this work): the test now holds the flushes.
- CI (workflow_dispatch on the branch, merged tree): threadpool run 41542646368 and scheduler run 41542683108, all jobs green. A first threadpool run on the merge failed Linux TSan on a relaxed counter in `work_pool.cc` (fixed: release increment).

## Review follow-up

- CR1: merged `origin/main` (conflicts in `AGENTS.md`, `CMakeLists.txt`); ctest 8/8 (post, soak, task_size, handshake_model, no_memory
  included), TSan ctest 7/7, the seven mutants again, the A/B above.
- CR4/condition 2: `docs/API.md` says not to destroy a `ThreadedScheduler` or `Debouncer` from one of its own tasks.
- CR6: a dispatch the pool cannot allocate is counted: `ThreadedScheduler::refused_size()`. A log line would allocate under the
  failure, so there is none. `test/no_memory.cc` injects a nothrow failure; the mutant "refusal not counted" fails it.
- CR7 goes to the lead: thread policies are ignored at run time.

## Step 4: repin on the threadpool without `ThreadPool` (branch `phase7c/drop-threadpool`)

- `CMakeLists.txt` pins threadpool `db38594` (branch `phase7c/drop-threadpool`, which deletes the `ThreadPool` family and trims
  `ThreadPolicyType` to `regular` and `logging`). The scheduler used neither; nothing else changes. `ThreadPolicyType::regular` is
  the only value any file here names.

## Next

After the review: threadpool steps 3 to 5 (delete the `ThreadPool` family, repin logger, strata, prospect and the other
consumers, then Xapiand on `deps/pins-2026-11`, the Detent note). Land threadpool and scheduler on main when the review clears.

## Blocked on

Nothing.
