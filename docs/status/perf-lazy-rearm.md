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
  not `[[nodiscard]]`. stash's `add` returns `stash_result` (pinned to stash `perf/window-epoch`
  until it lands), mapped one to one. A refused touch erases the key's status (the add runs under the status mutex, so a
  later touch on a pending call knows it is on the wheel); a refused fire-path re-arm runs the
  call on the fire thread and releases the key. Owner's refinement (past_horizon is an error,
  never run now): a fire-path re-arm refused as `past_horizon` drops the call, releases the key
  and reports through `L_DEBUG_HOOK`; `stopping` and `no_memory` still run it now. The queue's `walk`, `peep`, `clean` and
  `clean_checkpoint` are `noexcept`. `pending_keys()` added.
- Tests: `test_add_result` (scheduled, past_horizon, stopping), `test_debouncer_refusals`
  (past_horizon and stopping touches leave nothing pending; a re-arm refused past the horizon
  runs nothing and releases the key), `test/no_memory.cc` (no_memory from `add` and `debounce`,
  nothing pending, a pending call keeps its arguments; a re-arm refused for memory runs the call
  once, early, with the last arguments; a separate executable because it replaces `operator new`,
  skipped in TSan builds). Each fails with its fix reverted. TSan and ASan+UBSan, 3 runs.
- Downstream: Xapiand's 17 TUs that use the scheduler, a debouncer or the logger build against
  this branch's pins in a full configure of Xapiand's `fix/debounce-refusals` (no new diagnostics
  from these headers). Kronuz/logger `fix/refused-add` acts on a refused add instead of losing
  the line.

## Next

1. Code review of the debouncer change (the fire/re-arm critical section, the throttler release)
   and of the `bool` `add`.
2. If it lands: bump Xapiand's scheduler pin; consider the throttle path under churn.

- CI fix: `scheduler_no_memory_test` replaces global `operator new`, which TSan's static runtime
  also defines; CMake now skips it only when the C++ or linker flags (any config) carry
  `-fsanitize=thread` (CI passes TSan through `CMAKE_CXX_FLAGS_RELWITHDEBINFO`), so it still
  runs under ASan+UBSan. Checked with CI-shaped TSan and ASan configs locally.
- Owner's fail-fast decision on DW11: stash's `add` is `noexcept` and returns `stash_result
  { placed, past_horizon, no_memory }` (`no_memory` only before it claims a slot; a failure
  after the claim terminates). `SchedulerQueue::add` maps it one to one, with no `try`/`catch`
  around stash. stash repinned to its fail-fast commit.

## Blocked on

Nothing. The Debouncer refusal question is settled by the owner (above).

## For reviewers

- stash DW11 is fixed fail-fast on stash's side: a `std::bad_alloc` before the leaf claim is
  `stash_result::no_memory` with nothing claimed, which this queue maps to `no_memory`; one after
  the claim terminates the process. No hole can exist in a running process.
- The touch path holds the status mutex across the wheel add (lock order: status mutex, then
  the scheduler's; nothing takes them the other way; the reviewer's TSan probe agrees). Not
  re-measured on `bench_churn`.
- Code review conditions (`review-wt/stash/docs/reviews/window-refusals-code.md`): C1 fixed (a
  touch on a pending key after `finish()` returns `stopping` and releases the key;
  `test_debouncer_touch_after_finish` and the reviewer's `debouncer_probe.cc` fail on the
  previous code: `pending-after=scheduled ran=0`, 58-61 keys left pending; now `stopping`, 0
  left). CR2 fixed (a touch whose target would pass the horizon returns `past_horizon` and keeps
  the pending call; `test_debouncer_refusals` asserts the earlier call fires). Residual: a key
  whose task was pending when `finish()` began and is never touched again is still not run (the
  pool stops first); Xapiand's pool close commits writables.
