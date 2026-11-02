# scheduler

Part of [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries), a collection of small, reusable C++ libraries.

[![CI](https://github.com/Kronuz/scheduler/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/scheduler/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![Header-only](https://img.shields.io/badge/header--only-yes-green.svg)

Timer-wheel scheduler and per-key debouncer for C++20: cancellable tasks up to 24 hours ahead at
1 ms resolution. Tasks go into a lock-free four-level wheel ([stash](https://github.com/Kronuz/stash))
and one thread runs them when due, inline or on a worker pool; the debouncer collapses a burst of
calls for one key into a single call with the latest arguments. Reach for it for timeouts,
retries, deferred flushes and commits, and rate-limited background work.

## ✨ Highlights

- **A 24-hour wheel at 1 ms resolution.** Four levels (4,800 x 18 s, 36 x 500 ms, 10 x 50 ms,
  50 x 1 ms) on the lock-free stash slot store.
- **Cheap adds.** `add()` files a task into the wheel without a lock and takes the mutex only to
  wake a sleeping thread for an earlier deadline; `post()` hands a task to the thread with one
  compare-and-swap.
- **Cancellable tasks.** `clear()` is one compare-and-swap; the wheel skips a cleared task when
  it reaches it.
- **Two runners.** `Scheduler` runs due tasks inline on its thread; `ThreadedScheduler`
  dispatches them to a worker pool, so a slow task cannot stall the wheel.
- **Every add reports.** `add()` is `noexcept` and returns a `schedule_result`: `scheduled`, or
  why the task never fires (`past_horizon`, `no_memory`, `stopping`).
- **A debouncer** that coalesces rapid touches of a key into one call with the latest arguments,
  with a randomized force window so a key touched forever still fires and many keys due at once
  spread out, and an optional throttle: a floor on how often a key can fire.

## 🚀 Quick start

A task is a class deriving from `ScheduledTask` (CRTP) with an `operator()`; a debouncer deduces
its argument types from the callback:

```cpp
#include "debouncer.h"
#include "scheduler.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

using namespace std::chrono_literals;

struct Ping;
using Sched = Scheduler<Ping>;
struct Ping : ScheduledTask<Sched, Ping> {
	const char* name;
	explicit Ping(const char* name) : name(name) {}
	void operator()() { std::printf("%s\n", name); }  // runs on the scheduler thread when due
};

int main() {
	Sched sched("SCHED");
	auto now = std::chrono::steady_clock::now();
	auto ping = std::make_shared<Ping>("ping");
	auto never = std::make_shared<Ping>("never");
	sched.add(ping, now + 100ms);
	sched.add(never, now + 50ms);
	never->clear();  // cancelled: it does not run
	std::this_thread::sleep_for(200ms);
	sched.finish();

	auto deb = make_debouncer<int>("DEB", "DW{:02}", /*threads*/ 2,
		[](int value) { std::printf("settled at %d\n", value); },
		/*throttle*/ 0ms, /*timeout*/ 50ms, /*busy timeout*/ 50ms, /*force window*/ 150ms, 250ms);
	for (int v = 0; v < 100; ++v) { deb.debounce(/*key*/ 1, v); }
	std::this_thread::sleep_for(300ms);
	deb.finish();
}
// Output:
// ping
// settled at 99
```

[`examples/demo.cc`](examples/demo.cc) tours both: ordering, cancellation, a worker pool, and a
debounced burst.

## 📦 Installation

Requires a C++20 compiler and CMake 3.14 or newer. CMake fetches
[stash](https://github.com/Kronuz/stash), [threadpool](https://github.com/Kronuz/threadpool) and
[random](https://github.com/Kronuz/random) at pinned commits (random only when the build has no
`random::random` target yet). CI builds and tests it on Linux (x86_64 with GCC and Clang, arm64),
macOS (arm64, x86_64) and FreeBSD, plus ASan + UBSan and TSan builds.

```cmake
include(FetchContent)
FetchContent_Declare(scheduler
  GIT_REPOSITORY https://github.com/Kronuz/scheduler.git
  GIT_TAG        754ef84b3852cbffb8db112288c1b5cebcb9aa13)  # or main to track the tip
FetchContent_MakeAvailable(scheduler)

target_link_libraries(my_app PRIVATE scheduler::scheduler)
```

```cpp
#include "scheduler.h"  // ScheduledTask, Scheduler, ThreadedScheduler
#include "debouncer.h"  // Debouncer, make_debouncer
```

## 🎯 When to use it

Many timers or deferred jobs, cancelled often, at millisecond resolution: timeouts, deferred
commits and flushes, retries. Coalescing bursts per key: "fsync this file soon", "replicate this
shard soon".

Not for:

- Sub-millisecond deadlines or hard real-time: the wheel's resolution is 1 ms and tasks run on a
  regular thread.
- Tasks more than 24 hours out: `add` returns `schedule_result::past_horizon` for them, and they
  never fire.

## ⚡ Performance

`scheduler_bench_post --compare` times what a producer pays to arm a task due 1 ms later while
the scheduler thread runs the earlier ones: 200,000 tasks per producer, nanoseconds per task, the
range over two runs, each the median of five rounds. Every column includes making the task
(`std::make_shared`) and reading the clock, which *make only* times alone. *`add()`* files the
task into the wheel on the producer's thread; *`post()`* hands it to the scheduler thread, which
files it; *`post_ordered()`* hands it over to be kept in arrival order. The comparator, *queue*,
is the timer queue a caller writes without the library: a `std::priority_queue` of (due time,
arrival, task) under one `std::mutex`, and one thread that sleeps on a condition variable and
runs the due tasks in batches of up to 64 with the lock released, using libc++ as shipped with
Apple clang 17.0.0. Before timing, the bench checks that 300 tasks armed in shuffled order fire
in the same order from the wheel and from the queue (due time, then arming order).

| Machine, producers | make only | `add()` | `post()` | `post_ordered()` | queue |
|---|---:|---:|---:|---:|---:|
| Apple M4 Pro, 1 | 31 | 135 to 144 | 37 | 37 | 97 |
| Apple M4 Pro, 4 | 35 to 37 | 568 to 622 | 642 to 650 | 588 to 627 | 1,021 to 1,043 |
| Intel Core i9-9980HK, 1 | 128 to 129 | 373 to 380 | 124 to 127 | 120 to 126 | 267 to 277 |
| Intel Core i9-9980HK, 4 | 119 to 120 | 580 to 584 | 266 to 299 | 270 to 310 | 3,439 to 3,442 |

A task is 56 bytes (`sizeof(ScheduledTask)` on a 64-bit target) and does not derive
`std::enable_shared_from_this`; *make only* is what its allocation and construction cost every
caller, whichever call arms it. With one producer the queue's push is cheaper than `add()`, and
`post()` is cheaper than both; with four producers the queue's one mutex costs 1.7 times `add()`
on the M4 Pro and six times on the i9. Running 200,000 tasks due at one instant, from the first run
to the last, the queue is faster: 60 to 62 ns per task against 65 to 67 for the wheel on the M4 Pro,
and 115 to 119 against 232 to 235 on the i9.

Apple M4 Pro (arm64), macOS 26.7.2, and Intel Core i9-9980HK (x86_64), macOS 15.8.1, both with
Apple clang 17.0.0 and `-O3 -DNDEBUG`, at `e7b4f83`:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DSCHEDULER_BENCH=ON
cmake --build build --target scheduler_bench_post
./build/scheduler_bench_post --compare
```

## 📚 Documentation

- [docs/API.md](docs/API.md): `ScheduledTask`, `Scheduler`, `ThreadedScheduler`, `Debouncer`, and
  the tracing hooks.
- [ARCHITECTURE.md](ARCHITECTURE.md): the wheel, the thread loop, the two runners, and the
  debouncer's timing model.
- [docs/perf/](docs/perf/): the debouncer's memory under cancel churn, with method and numbers.
- [CHANGELOG.md](CHANGELOG.md): dated changes and their measurements.
- [AGENTS.md](AGENTS.md): repository map, invariants, and traps.
- [examples/colored_trace/](examples/colored_trace/README.md): plugging in colored tracing.
- [Catalog entry](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md#scheduler) in Kronuz
  C++ Libraries.

## 🛠️ Building and testing

```sh
cmake -B build && cmake --build build && ctest --test-dir build
./build/scheduler_demo
cmake -B build -DSCHEDULER_BENCH=ON && cmake --build build
```

The tests are timing-based with generous windows; they also check that a touch arriving at the
debouncer's force deadline still delivers its value.

`scheduler_soak [adds] [producers] [idle_ms] [mode]` (built with the tests) is the lost-task soak:
it adds tasks while the worker alternates between running and sleeping, and exits non-zero if any
task fails to fire. `mode 1` adds bursts of 20,000 tasks right as the worker goes back to sleep. A
short run is a CTest test; run it with 100M adds to soak.

`SCHEDULER_BENCH` builds `scheduler_bench_churn` (the debouncer under cancel churn) and
`scheduler_bench_post` (what a producer pays to arm a task with `add()`, `post()` and
`post_ordered()`; `--compare` adds the `std::priority_queue` timer queue).

## 🤝 Contributing

Issues and pull requests are welcome. Read [AGENTS.md](AGENTS.md), which lists the invariants a
change must keep, and the catalog's
[rules of engagement](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md#rules-of-engagement).

## 📄 License

MIT. See [LICENSE](LICENSE). Extracted from [Xapiand](https://github.com/Kronuz/Xapiand)
(Copyright © 2015-2020 Dubalu LLC), where it drives the logger thread and every debounced fsync,
commit, and replication trigger.
