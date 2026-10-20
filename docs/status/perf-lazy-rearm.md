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
- Owner's follow-up, no silent drops: `try_add` (SchedulerQueue and BaseScheduler) returns false
  past the horizon, `dropped()` counts every horizon refusal, `add` keeps its behaviour and is
  counted; `test_past_horizon`, which fails with the count removed. Debug, TSan and ASan+UBSan
  suites pass (3 runs each under the sanitizers).

## Next

1. Code review of the debouncer change (the fire/re-arm critical section, the throttler release)
   and of `try_add`/`dropped()`.
2. If it lands: bump Xapiand's scheduler pin; consider the throttle path under churn.

## Blocked on

Nothing.
