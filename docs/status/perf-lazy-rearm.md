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
- Owner's follow-up, no silent drops (f92ba00 tried `try_add` and a `dropped()` count; eca6062 a
  `bool`; the owner settled on an enum): every `add` overload and `debounce` / `delayed_debounce`
  are `noexcept` and return `schedule_result { scheduled, past_horizon, stopping, no_memory }`,
  not `[[nodiscard]]`. stash's `add` stays `bool` (pinned to stash `perf/window-epoch` d234d24
  until it lands); the scheduler maps false to `past_horizon` and `std::bad_alloc` to
  `no_memory`. A refused touch erases the key's status (the add runs under the status mutex, so a
  later touch on a pending call knows it is on the wheel); a refused fire-path re-arm runs the
  call on the fire thread and releases the key. The queue's `walk`, `peep`, `clean` and
  `clean_checkpoint` are `noexcept`. `pending_keys()` added.
- Tests: `test_add_result` (scheduled, past_horizon, stopping), `test_debouncer_refusals`
  (past_horizon and stopping touches leave nothing pending; a refused re-arm runs once, with the
  last arguments), `test/no_memory.cc` (no_memory from `add` and `debounce`, nothing pending, a
  pending call keeps its arguments; a separate executable because it replaces `operator new`,
  skipped in sanitized builds). Each fails with its fix reverted. TSan and ASan+UBSan, 3 runs.
- Downstream: Xapiand's scheduler, debouncer and logger TUs (manager, handler, discovery, shard,
  wal, pool, main, logger.cc) compile against this branch, stash d234d24 and the logger branch
  (`-fsyntax-only` from an existing Xapiand build, no new diagnostics from these headers). Kronuz/logger
  `fix/refused-add` (a48bce8) now acts on a refused add instead of losing the line.

## Next

1. Code review of the debouncer change (the fire/re-arm critical section, the throttler release)
   and of the `bool` `add`.
2. If it lands: bump Xapiand's scheduler pin; consider the throttle path under churn.

## Blocked on

Nothing. The Debouncer refusal question is settled by the owner (above).

## For reviewers

- stash DW11 is now reachable: `no_memory` keeps the process alive, but a `std::bad_alloc`
  inside stash after a leaf slot is reserved (the value box, or a leaf chunk) leaves a permanent
  reserved hole that pins `first_valid`, so the wheel stops reclaiming and, 24 h later, refuses
  every add as `past_horizon`. Before, the exception propagated out of `add`. A fix belongs in
  stash (mark the reserved slot drained on failure), not here.
- The touch path now holds the status mutex across the wheel add (lock order: status mutex, then
  the scheduler's; nothing takes them the other way). Not re-measured on `bench_churn`.
