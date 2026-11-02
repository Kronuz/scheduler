# Code review: phase7c/unify-pool (threadpool + scheduler)

Reviewer: code-reviewer, 2026-11-02. Covers `threadpool:44551e5`/`threadpool:d716206` and `scheduler:157d66d`
(both branches `phase7c/unify-pool`).

## Verdict: APPROVE WITH CONDITIONS

The code is sound: no race, lifetime or loss finding in the new ThreadedScheduler, and the semantics change matches what
the old pool already did. One blocker for landing: the scheduler branch is 27 commits behind main and its CI proof is on the
stale base. Conditions below.

## Blocking

CR1. Scheduler branch is based on `scheduler:2cbea95` (2026-10-20); `origin/main` is `scheduler:2bb0a8f`, 27 commits ahead
(post()/post_ordered(), hand-off fields, the 56-byte task without `enable_shared_from_this`, new tests `post.cc`,
`handshake_model.cc`, `soak.cc`, `task_size.cc`, README/CHANGELOG rewrites). `git merge-tree HEAD origin/main`: text conflicts in
`AGENTS.md` and `CMakeLists.txt`; `scheduler.h`, `test/test.cc`, `test/no_memory.cc` auto-merge. The green CI run, the sanitizer
runs, the mutants and the A/B bench (base `2cbea95`) all describe the pre-main tree, so none of them is evidence for the merged
tree. Needed: merge or rebase onto main, resolve the two conflicts, rerun scheduler ctest (including main's `post`, `soak`,
`task_size`, `no_memory`) and CI on the result, and refresh the A/B base to main's head (main's `add()` path changed, so the
`add()` row in `docs/status/unify-pool/ab.txt` is not comparable). Evidence that the merge is probably clean: the
auto-merged `scheduler.h` (`.scratch/review-unify/merged/scheduler.h`) compiles and passes my probes below (`probe-merged`).
The threadpool branch is 2 commits behind main (bench and a doc only), merges clean; nothing to do but merge.

## Findings (non-blocking)

CR2. Semantics: dropping unstarted tasks at finish is not new. The old `ThreadPool::finish()` set `_finished`, and workers
exit on `!_finished` before popping (`threadpool.hh:399`), so queued tasks never ran; `enqueue` after finish threw
`logic_error`, caught and dropped in the old `operator()`. New behavior is the same for tasks, with a bounded window: a
dispatch that the wheel passed `stopped` for just before `finish()`'s `group.cancel()` can still run (`scheduler.h` finish:
`stopped=true` then `cancel()`; `operator()` checks `stopped` then enqueues). `join()` cancels again after the wheel exits, so
with a join nothing runs after join returns. Only a bare `finish(0)` can run one straggler; harmless.

CR3. Consumers. Logger does not use ThreadedScheduler: `logger.cc:504` is a plain `Scheduler<Logging, logging>` (runs on the
wheel thread), unchanged by this branch, so L_INFO flush at exit is not affected. Xapiand's users are the Debouncers
(`xapiand_fsyncher.h:52`, committer; `manager.cc:1110-1135` finish then `join(500ms)` loop, reads `running_size()` and
`threadpool_size()`/`threadpool_capacity()` at `manager.cc:1258-1261`). Their shutdown dropped unstarted fsync/commit tasks
before too (CR2), so no new loss. `threadpool_size()` now returns `pool.threads()` (configured count), the old value was live
worker count; equal in steady state, not during startup/teardown. Metrics only.

CR4. Destruction from a task of the same scheduler now aborts: `~WorkGroup` calls `check_stack` then `wait()`
(`work_pool.hh:1330`), which aborts when an item of the group is on the stack. Old `~ThreadPool` from a worker would have
deadlocked or thrown on self-join, so this is not a regression in kind, but it is a new hard failure mode: worth a line in
`docs/API.md` ("do not destroy a ThreadedScheduler/Debouncer from one of its own tasks"). Also `finish(wait != 0)` called from
inside a task still times out (as the ledger says) because that task keeps the group busy.

CR5. `~ThreadedScheduler` is `finish(1)` (join 200 ms) and then members are destroyed; `~WorkGroup` waits without a timeout for
running tasks (probe P3: a 600 ms task, destructor took 551 ms, no UAF, no TSan report). The old destructor also joined threads
without bound, so no change, but a task that never ends blocks destruction forever.

CR6. `group.enqueue` can return false on box allocation failure (nothrow new); `operator()` ignores it, so the task is
claimed (`clear(true)`) and silently lost. Old code threw `bad_alloc` out of the wheel thread (worse). Consider a trace line;
not blocking.

CR7. Design note for the architect: `ThreadPolicyType` is now ignored at run time for the pool's workers (ledger says so),
so `ThreadPolicyType::fsynchers` etc. on Xapiand Debouncers no longer select a policy. Was true of `setup_thread` before; just
confirm that nobody expects distinct scheduling classes.

## What I checked (probes in `scheduler-wt/unify-pool/.scratch/review-unify/`)

- `probe.cc`, built TSan (Homebrew LLVM `-O1`) by `build.sh`, binaries `probe-tsan` (branch) and `probe-merged` (branch
  `ThreadedScheduler` with main's `scheduler.h` auto-merged); outputs `probe-tsan.out`, `probe-merged.out`. No TSan reports,
  no hangs, no UAF:
  - P1 (300 rounds): tasks that re-add tasks while another thread calls `finish(0..2)` and the main thread `join(2s)`, then
    `delete`. All joins returned true. Covers finish/join racing in-flight dispatch and task-adds-tasks during finish (`add()`
    returns `stopping` once `ending >= 0`).
  - P2 (3000 rounds): `finish(0)` then `join(0..2 ms)` with tasks due; races the `IdleWait` callback against timeout, including
    the `cancel_idle` false path. The stack node is never used after destruction: the joiner re-takes `done.mtx`, which the
    callback holds while setting `idle`.
  - P3: destructor with a running 600 ms task (CR5).
  - P4: `WorkGroup` with 4 workers, 200,000 enqueues, a sampler thread reading `running()`/`pending()`: `running() <= 4`
    always, `pending()` bounded, both 0 after `wait()`. By reading, `pop` and `++running` are one hold of `s.mu`
    (`work_pool.hh:878-880`) and `--running` is under it again, so a task is never counted in neither or both; the two getters
    are separate lock holds, as the header says.
- `on_idle`/`cancel()`: `cancel_items` takes the idle list only if the group is idle (`take_idle`), so `join`'s second cancel
  followed by `on_idle` cannot lose a wakeup: either the group is idle now (callback inline) or the last finisher fires it.
- threadpool `work_group.cc` case 4 fix: flushes now wait (bounded 5 s) for the query to set `seen`; correct, and it removes
  the timing dependence. `work_counts.cc` plus the two mutants in `test/mutants.sh` die as the ledger says (read, not rerun).

## Not run

Full ctest of either repo, ASan/UBSan, FreeBSD/Linux (CI per the lead), the ledger mutants and the A/B bench (benchmarks
not allowed), a merge-and-build of the whole merged tree (only `scheduler.h` was compiled, in the probe; CR1 asks for the
rest), Xapiand at its head against the new scheduler (that is the repin step). The host was under exclusive holds for about
50 minutes, which is why the probes are few.

## Conditions to land

1. CR1: merge/rebase scheduler onto main, resolve `AGENTS.md`/`CMakeLists.txt`, green ctest and CI on the merged head, refreshed
   A/B base; threadpool merges main (clean).
2. CR4: add the API.md line (doc only).
