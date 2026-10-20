# Status: perf/lazy-rearm

## State

Done, ready for review with the past-horizon follow-up; not merged. Results in
`docs/perf/lazy-rearm.md`.

## Done

- `bench/bench_churn.cc` (b4f4895, 9751d22, 93d6f9e; made opt-in in the CI fix), the re-arm in
  `debouncer.h` and `test_debouncer_rearm` (b4f4895), docs (925cd52).
- Churn A/B, three interleaved rounds: on 1,000 hot keys 507,000 tasks alive and 99-108 MB of
  heap become 1,002 and 1.5 MB; 100,000 keys halve; every key fires its last touch.
- TSan and ASan clean locally; CI green on all 8 jobs (run 39382526110).
- Owner's follow-up, no silent drops (f92ba00 first added `try_add` and a `dropped()` count; the
  owner replaced both): every `add` overload is `noexcept` and returns `bool`, false past the
  horizon (stash's own `add` now returns false there; pinned to stash `perf/window-epoch`
  d234d24 until that lands), on `std::bad_alloc` inside stash, and after `finish()` began. The
  queue's `walk`, `peep`, `clean` and `clean_checkpoint` are `noexcept`. `test_add_result` checks a
  past-horizon add returns false and never fires, an add during shutdown returns false, and a
  normal add returns true and fires.

- Downstream: Xapiand's scheduler, debouncer and logger TUs (manager, handler, discovery, shard,
  wal, pool, main, logger.cc) compile against this branch, stash d234d24 and the logger branch
  (`-fsyntax-only` from an existing Xapiand build, no new diagnostics from these headers). Kronuz/logger
  `fix/refused-add` (a48bce8) now acts on a refused add instead of losing the line.

## Next

1. Code review of the debouncer change (the fire/re-arm critical section, the throttler release)
   and of the `bool` `add`.
2. If it lands: bump Xapiand's scheduler pin; consider the throttle path under churn.

## Blocked on

- Owner decision, not blocking this review: the Debouncer ignores `add`'s result at
  `debouncer.h:179` (the fire path's re-add of a re-armed or throttler task) and `debouncer.h:293`
  (a touch's new task). Its timeouts are seconds, so only shutdown (or OOM) refuses; then the key
  keeps a task that never fires, and the pending call is lost: a committer commit, an fsyncher
  fsync, a db, schema, settings or primary update broadcast, or a replication trigger touched while
  Xapiand shuts down. A refused throttler loses nothing. This predates the branch (main's `add`
  ignored the task once `finish()` began). Options: run the call inline on refusal (as the logger
  now writes due lines), or drop the key's status so nothing stays stuck. Recommendation: run the
  re-armed call inline in the fire path (already on a worker), and in `debounce()` run it on the
  caller only when the scheduler is ending; needs the owner's call because it moves user callbacks
  onto producer threads.
