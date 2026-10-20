# Lazy re-arm for the debouncer

Branch `perf/lazy-rearm` from main `fb464cb`. Prototype for review, not merged.

## Target

Memory under cancel churn. Before this branch every `debounce(key, args)` on a key with a pending
call cleared that task and added a new one. stash has no remove, so each cleared task stayed in the
wheel, with its heap box, the `DebouncerTask`, the key and the arguments, until the walk drained its
slot at the old deadline. Xapiand runs seven debouncers on this code (the committer,
`trigger_replication`, the fsyncher, and the db, schema, settings and primary updaters); the
committer is touched on every write to a shard.

## Hypothesis

A touch on a pending key needs to move one time and replace one set of arguments, not allocate a
task and add it. Keep the target and arguments in the key's `Status` (under the existing mutex);
the pending task, when it fires before the target, re-adds a fresh task at the target. Expected:
cancelled tasks alive drop from about rate times delay to zero, with the same calls fired. Keep it
if memory drops materially, every call still fires with the last arguments, and the sanitizers stay
clean; remove it if the semantics change or CPU rises beyond the memory it saves.

## Method

`bench/bench_churn.cc` (`-DSCHEDULER_BENCH=ON`): the real `Debouncer` built against stash main `a41b2d0`
(`FETCHCONTENT_SOURCE_DIR_STASH`), `-O3 -DNDEBUG`, two producer threads touching random keys at
100,000 touches/s in total for 30 s, then 12 s with no touches. Timeouts are the committer's
defaults (`opts.cc`): debounce 1 s, busy 3 s, forced within 8 to 10 s, no throttle. Each argument
set is a `weak_ptr` plus a key and sequence, and counts itself. Heap is the usable size of every
live allocation (global `operator new` replaced); RSS is the kernel's; CPU is the process's user
plus system time. Each thread owns its own keys, and after the drain every key must have fired
with its last touch's sequence (the bench exits nonzero otherwise).

Runs were interleaved main, re-arm, main, re-arm for each key count, three rounds in all (the
first without the CPU counter and the sequence check, the third with both), under light holds on
macOS x86_64 (i9-9980HK). Logs: `.scratch/logs/churn_ab.log`, `churn_cpu.log`, `churn_final.log`.

## Results

Peaks over each run; ranges over all runs of that cell (6 for memory, 4 for CPU).

| 100,000 touches/s, 30 s | main | re-arm | change |
|---|---|---|---|
| 1,000 keys: tasks alive | 506,892-507,300 | 1,002 | 506x fewer |
| 1,000 keys: heap | 99.1-107.9 MB | 1.3-1.6 MB | 98.5% less |
| 1,000 keys: RSS | 238.6-253.0 MB | 4.9-5.1 MB | 98% less |
| 1,000 keys: CPU per 42 s run | 10.6-12.0 s (median 11.5) | 2.6-3.6 s (median 3.2) | 3.6x less |
| 1,000 keys: calls fired | 4,000-4,003 | 4,000 | same |
| 100,000 keys: tasks alive | 282,192-283,121 | 92,269-92,385 | 3.1x fewer |
| 100,000 keys: heap | 71.2-78.4 MB | 37.0-38.9 MB | 50% less |
| 100,000 keys: RSS | 122.7-170.6 MB | 57.3-64.2 MB | 55-62% less |
| 100,000 keys: CPU per 42 s run | 18.7-21.8 s (median 19.5) | 19.6-23.4 s (median 21.4) | about 10% more, ranges overlap |
| 100,000 keys: calls fired | 519,436-520,350 | 518,961-520,087 | same |
| Keys whose last touch never fired (checked runs) | 0 | 0 | |

On 1,000 hot keys main's dead tasks swing between 100,000 and 507,000 (about 2.5 s of touches on
average) as the busy and force deadlines drain them, at about 200 B each, and RSS only climbs
because the allocator keeps what it freed. With the re-arm what stays alive is one pending task per
key. On 100,000 keys, touched about once a second each, the re-armed figure is the live state (one
status and one pending task per touched key, about 92,000), and main carries about 190,000 dead
tasks on top of it.

CPU: on hot keys each touch now costs a map lookup and an argument move instead of an allocation
and a wheel add: about 1.1 µs of process CPU per touch against 3.8 µs (fires and walks included). On 100,000 lukewarm keys the
fires dominate (520,000 pool dispatches either way), and the re-arm may cost a little more: a call
touched while pending now fires early once, re-adds itself, and fires again, a second pool dispatch
that main avoided by skipping the cleared task inline. The four runs per side overlap, so this is a
possible cost of up to about 10%, not a measured one.

At the stated scale (100,000 touches/s, the committer's timeouts) the saving is 100 to 250 MB per
debouncer on hot keys and about 0.2 of a core. At Xapiand's usual write rates (a few thousand touches
a second) the same pathology is under 2 MB, negligible; it matters for bulk ingest, where the
committer sees one touch per write.

## Correctness

- `test/test.cc` gains `test_debouncer_rearm`: 1,000 touches of a pending key keep at most two
  argument sets alive and fire once with the last value. On the base `debouncer.h` it fails
  ("pending touches piled up").
- The rest of the suite (collapse, force window, cancel after dispatch, teardown) passes.
- TSan and ASan+UBSan (Homebrew clang 23.1.2): the test suite 3 times each, and `bench_churn` under
  TSan twice with 5/20/60 ms timeouts on 50 keys (320,000 touches racing 14,000 fires), with no
  reports.
- stash's concurrency is untouched (`add` is unchanged; the touch and the fire synchronize on the
  debouncer's mutex), so no stash interleaving model applies. The one new ordering, a touch landing
  between the fire's decision and its release, is closed by doing both in one critical section.
- A pre-existing race is also closed: the throttler used to erase the key's status unconditionally,
  so a touch that turned the key back into a pending call between the throttler's dispatch and its
  run lost that call. It now erases only while it is still the key's task.

## Not covered

- Linux and arm64 (CI is dispatched; no timed run there).
- The throttle path under churn (the committer has no throttle; the updaters do).
- A stash-side payload reaper: not needed here, since nothing dead is left to reap.
