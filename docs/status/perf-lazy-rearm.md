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

## Next

1. Code review of the debouncer change (the fire/re-arm critical section, the throttler release)
   and of the `bool` `add`.
2. If it lands: bump Xapiand's scheduler pin; consider the throttle path under churn.

## Blocked on

Nothing.
