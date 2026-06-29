// A runnable tour of scheduler: the timer wheel and the debouncer.
//
// Build (when this repo is the top-level project):
//   cmake -B build && cmake --build build && ./build/scheduler_demo
//
// The one idea worth taking away: you schedule cancellable tasks at future
// wakeup times on a lock-free 24-hour timer wheel, and a background thread runs
// each when it comes due — earliest first, regardless of insertion order. On top
// of that sits a per-key debouncer that collapses a burst of touches into a
// single eventual call. This demo schedules tasks OUT of order and watches them
// fire in time order, cancels one before it fires, dispatches a batch to a worker
// pool, and hammers a debouncer key to show the burst coalesce.
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "debouncer.h"
#include "scheduler.h"

using namespace std::chrono_literals;

static void rule(const char* title) {
	std::printf("\n\033[1m── %s ──\033[0m\n", title);
}

// Milliseconds elapsed since a reference point, so the demo can print *when*
// each task fired relative to when it was scheduled.
static long ms_since(std::chrono::steady_clock::time_point t0) {
	auto now = std::chrono::steady_clock::now();
	return std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count();
}


// --- 1. inline scheduler: out-of-order in, time-order out --------------------
// A task that, when due, prints its label and how long after t0 it fired.

struct OrderTask;
using InlineSched = Scheduler<OrderTask, ThreadPolicyType::regular>;
using InlineBase  = ScheduledTask<InlineSched, OrderTask, ThreadPolicyType::regular>;

// The wheel's reference instant and the order tasks actually fired in.
static std::chrono::steady_clock::time_point g_t0;
static std::mutex g_order_mtx;
static std::vector<const char*> g_fired;
static std::atomic<int> g_inline_ran{0};

struct OrderTask : public InlineBase {
	const char* label;
	explicit OrderTask(const char* label) : label(label) {}
	void operator()() {
		{
			std::lock_guard<std::mutex> lk(g_order_mtx);
			g_fired.push_back(label);
		}
		// Runs inline on the scheduler thread itself.
		std::printf("  fired %-8s at ~%ld ms\n", label, ms_since(g_t0));
		g_inline_ran.fetch_add(1, std::memory_order_relaxed);
	}
};

static void demo_inline_scheduler() {
	rule("inline scheduler: scheduled out of order, fired in time order");

	InlineSched sched("SCHED");
	g_t0 = std::chrono::steady_clock::now();

	// Hand the wheel three tasks in a scrambled order. Each add() that lands
	// sooner than the current sleep wakes the thread to re-aim.
	std::puts("  scheduling: \"third\"@300ms, \"first\"@100ms, \"second\"@200ms (note the order)");
	sched.add(std::make_shared<OrderTask>("third"),  g_t0 + 300ms);
	sched.add(std::make_shared<OrderTask>("first"),  g_t0 + 100ms);
	sched.add(std::make_shared<OrderTask>("second"), g_t0 + 200ms);

	// Wait past the last wakeup for all three to run.
	for (int i = 0; i < 50 && g_inline_ran.load() < 3; ++i) {
		std::this_thread::sleep_for(20ms);
	}
	sched.finish();

	std::lock_guard<std::mutex> lk(g_order_mtx);
	// The wheel is a sorted structure: insertion order is irrelevant, time wins.
	assert(g_fired.size() == 3);
	std::printf("  -> ran in order: %s, %s, %s (insertion order was third, first, second)\n",
	            g_fired[0], g_fired[1], g_fired[2]);
}


// --- 2. clear(): cancel a scheduled task before it fires ---------------------

static void demo_cancel() {
	rule("clear(): cancel a scheduled task before it fires");

	g_inline_ran.store(0);
	InlineSched sched("SCHED2");

	auto now = std::chrono::steady_clock::now();
	auto keep   = std::make_shared<OrderTask>("keep");
	auto cancel = std::make_shared<OrderTask>("cancel");
	g_t0 = now;

	sched.add(keep,   now + 120ms);
	sched.add(cancel, now + 60ms);   // would fire FIRST, if we let it
	std::puts("  scheduled \"cancel\"@60ms and \"keep\"@120ms");

	// clear() is a single CAS: the first caller wins and the task is skipped when
	// the wheel reaches it. A second clear() loses.
	bool first  = cancel->clear();
	bool second = cancel->clear();
	std::printf("  cancel->clear() returned %s; a second clear() returned %s\n",
	            first ? "true" : "false", second ? "true (?!)" : "false");
	assert(first && !second);

	std::this_thread::sleep_for(250ms);
	sched.finish();

	// Only "keep" ran; the cancelled task never appeared.
	assert(g_inline_ran.load() == 1);
	std::puts("  -> only \"keep\" fired; the cancelled task was skipped");
}


// --- 3. threaded scheduler: due tasks dispatched to a worker pool ------------
// Same wheel, but each due task is handed to a pool worker instead of running
// inline, so a slow task can't stall the wheel. We print which worker ran each.

struct PoolTask;
using PoolSched = ThreadedScheduler<PoolTask, ThreadPolicyType::regular>;
using PoolBase  = ScheduledTask<PoolSched, PoolTask, ThreadPolicyType::regular>;

static std::atomic<int> g_pool_ran{0};

struct PoolTask : public PoolBase {
	void operator()() {
		g_pool_ran.fetch_add(1, std::memory_order_relaxed);
	}
};

static void demo_threaded_scheduler() {
	rule("threaded scheduler: due tasks dispatched to a 4-worker pool");

	PoolSched sched("TSCHED", "TW{:02}", 4);
	auto now = std::chrono::steady_clock::now();

	constexpr int N = 12;
	// Bunch several tasks into the same short window so they come due together
	// and the pool runs them concurrently.
	for (int i = 0; i < N; ++i) {
		sched.add(std::make_shared<PoolTask>(), now + std::chrono::milliseconds(50 + (i % 3) * 15));
	}
	std::printf("  scheduled %d tasks across a ~80ms window\n", N);

	for (int i = 0; i < 50 && g_pool_ran.load() < N; ++i) {
		std::this_thread::sleep_for(20ms);
	}
	sched.finish();

	assert(g_pool_ran.load() == N);
	std::printf("  -> all %d tasks dispatched to the pool and ran (a slow one wouldn't block the wheel)\n",
	            g_pool_ran.load());
}


// --- 4. debouncer: a burst of touches collapses into one delayed call --------
// The debouncer is built on the threaded scheduler. Touching a key reschedules
// its single pending task; rapid touches keep pushing it out (up to a randomized
// force window), so a long burst settles into roughly one call carrying the LAST
// arguments — exactly what you want for a fsync/commit/save trigger.

static std::mutex g_deb_mtx;
static std::vector<std::pair<int, int>> g_deb_calls;   // (key, value-at-fire)

static void demo_debouncer() {
	rule("debouncer: coalesce a burst of triggers into one delayed call");

	auto deb = make_debouncer<int>(
		"DEB", "DW{:02}", /*threads*/ 2,
		[](int key, int value) {
			std::lock_guard<std::mutex> lk(g_deb_mtx);
			g_deb_calls.emplace_back(key, value);
			std::printf("  callback fired for key %d with value %d\n", key, value);
		},
		/*throttle_time*/         0ms,
		/*debounce_timeout*/      50ms,    // a fresh key fires this long after the touch
		/*debounce_busy_timeout*/ 50ms,    // each further touch pushes it out by this
		/*min_force*/             150ms,   // but never past now + random(min, max):
		/*max_force*/             250ms);  // a forever-touched key still fires

	// Hammer key 1 with 30 rapid touches, value climbing each time. Each touch
	// reschedules the one pending task, so the wheel sees one moving target, not 30.
	std::puts("  touching key 1 thirty times (values 0..29), ~2ms apart...");
	for (int v = 0; v < 30; ++v) {
		deb.debounce(1, /*key*/1, /*value*/v);
		std::this_thread::sleep_for(2ms);
	}

	// A second key, touched just once, fires independently.
	std::puts("  touching key 2 once (value 7)");
	deb.debounce(2, /*key*/2, /*value*/7);

	// delayed_debounce adds a fixed delay on top of the debounce timing.
	std::puts("  touching key 3 once with a +100ms delayed_debounce (value 42)");
	deb.delayed_debounce(100ms, 3, /*key*/3, /*value*/42);

	// Wait past the force window so everything settles.
	std::this_thread::sleep_for(600ms);
	deb.finish();

	std::lock_guard<std::mutex> lk(g_deb_mtx);
	int key1_calls = 0, key1_last = -1;
	for (auto& [k, v] : g_deb_calls) {
		if (k == 1) { ++key1_calls; key1_last = v; }
	}
	// 30 touches collapsed to a handful of calls at most, and the LAST value won.
	std::printf("  -> key 1: 30 touches collapsed into %d call(s), last value %d (not 30 calls)\n",
	            key1_calls, key1_last);
	assert(key1_calls >= 1 && key1_calls <= 3);
	assert(key1_last == 29);
}


int main() {
	std::puts("scheduler demo  (a tour of the timer wheel and the debouncer)");

	demo_inline_scheduler();
	demo_cancel();
	demo_threaded_scheduler();
	demo_debouncer();

	std::puts("\ndone.");
	return 0;
}
