// Smoke test for the standalone scheduler library.
//
// Exercises the three things a consumer relies on:
//   1. Scheduler (inline): schedule tasks at future wakeup times and verify they
//      fire, in wakeup order, on the scheduler thread itself.
//   2. ThreadedScheduler: schedule tasks and verify they fire, dispatched to a
//      worker pool (so a slow task can't stall the wheel).
//   3. Debouncer: hammer a key with rapid calls and verify they collapse into a
//      single eventual call carrying the last arguments; separate keys fire
//      independently.
//
// The scheduler is timing-based, so the test uses generous windows (tens to
// hundreds of ms) and never asserts tight races.
//
// Build via CMake: cmake -B build && cmake --build build && ctest --test-dir build
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


// ---------------------------------------------------------------------------
// 1. Scheduler (inline): a task that records the order in which it fired.
// ---------------------------------------------------------------------------

struct OrderTask;
using InlineSched = Scheduler<OrderTask, ThreadPolicyType::regular>;
using InlineTaskBase = ScheduledTask<InlineSched, OrderTask, ThreadPolicyType::regular>;

// Shared sink the tasks append their id to as they run.
static std::mutex g_order_mtx;
static std::vector<int> g_order;
static std::atomic<int> g_inline_ran{0};

struct OrderTask : public InlineTaskBase {
	int id;
	explicit OrderTask(int id) : id(id) {}
	void operator()() {
		{
			std::lock_guard<std::mutex> lk(g_order_mtx);
			g_order.push_back(id);
		}
		g_inline_ran.fetch_add(1, std::memory_order_relaxed);
	}
};

static void test_inline_scheduler() {
	InlineSched sched("SCHED");

	auto now = std::chrono::steady_clock::now();

	// Add three tasks OUT of wakeup order; they must fire IN wakeup order.
	auto t_late   = std::make_shared<OrderTask>(3);
	auto t_early  = std::make_shared<OrderTask>(1);
	auto t_middle = std::make_shared<OrderTask>(2);

	sched.add(t_late,   now + 300ms);
	sched.add(t_early,  now + 100ms);
	sched.add(t_middle, now + 200ms);

	// Wait well past the last wakeup time for all three to run.
	for (int i = 0; i < 100 && g_inline_ran.load() < 3; ++i) {
		std::this_thread::sleep_for(20ms);
	}

	sched.finish();

	std::lock_guard<std::mutex> lk(g_order_mtx);
	assert(g_order.size() == 3);
	assert(g_order[0] == 1 && g_order[1] == 2 && g_order[2] == 3);
	std::printf("scheduler OK: 3 tasks fired in wakeup order: %d %d %d\n",
	            g_order[0], g_order[1], g_order[2]);
}


// ---------------------------------------------------------------------------
// 1b. Scheduler: clear() cancels a task so it never runs.
// ---------------------------------------------------------------------------

static void test_clear_cancels() {
	g_inline_ran.store(0);
	InlineSched sched("SCHED2");

	auto now = std::chrono::steady_clock::now();
	auto t = std::make_shared<OrderTask>(99);
	sched.add(t, now + 150ms);

	// Cancel before it can fire.
	bool cleared = t->clear();
	assert(cleared);            // first clear wins the CAS
	assert(!t->clear());        // second clear loses (already cleared)

	std::this_thread::sleep_for(300ms);
	sched.finish();

	assert(g_inline_ran.load() == 0);   // the cancelled task never ran
	std::printf("scheduler OK: cleared task did not fire\n");
}


// ---------------------------------------------------------------------------
// 2. ThreadedScheduler: tasks dispatched to a worker pool.
// ---------------------------------------------------------------------------

struct CountTask;
using ThreadedSched = ThreadedScheduler<CountTask, ThreadPolicyType::regular>;
using ThreadedTaskBase = ScheduledTask<ThreadedSched, CountTask, ThreadPolicyType::regular>;

static std::atomic<int> g_threaded_ran{0};

struct CountTask : public ThreadedTaskBase {
	void operator()() {
		g_threaded_ran.fetch_add(1, std::memory_order_relaxed);
	}
};

static void test_threaded_scheduler() {
	ThreadedSched sched("TSCHED", "TW{:02}", 4);

	auto now = std::chrono::steady_clock::now();
	constexpr int N = 20;
	for (int i = 0; i < N; ++i) {
		auto t = std::make_shared<CountTask>();
		// Spread wakeups across a short window.
		sched.add(t, now + std::chrono::milliseconds(50 + (i % 5) * 20));
	}

	for (int i = 0; i < 100 && g_threaded_ran.load() < N; ++i) {
		std::this_thread::sleep_for(20ms);
	}

	sched.finish();

	assert(g_threaded_ran.load() == N);
	std::printf("threaded scheduler OK: %d/%d tasks dispatched and ran\n",
	            g_threaded_ran.load(), N);
}


// ---------------------------------------------------------------------------
// 3. Debouncer: rapid same-key calls collapse to a single eventual call.
// ---------------------------------------------------------------------------

static std::mutex g_deb_mtx;
static std::vector<std::pair<int, int>> g_deb_calls;   // (key, last value)

static void test_debouncer() {
	// debounce_timeout 50ms, busy 50ms, force window 150..250ms, no throttle.
	auto deb = make_debouncer<int>(
		"DEB", "DW{:02}", 2,
		[](int key, int value) {
			std::lock_guard<std::mutex> lk(g_deb_mtx);
			g_deb_calls.emplace_back(key, value);
		},
		/*throttle_time*/            0ms,
		/*debounce_timeout*/         50ms,
		/*debounce_busy_timeout*/    50ms,
		/*debounce_min_force*/       150ms,
		/*debounce_max_force*/       250ms);

	// Hammer key 1 with 30 rapid touches; only the last value (29) should win.
	for (int v = 0; v < 30; ++v) {
		deb.debounce(1, 1, v);
		std::this_thread::sleep_for(2ms);   // fast enough to keep pushing it out
	}

	// Touch key 2 once.
	deb.debounce(2, 2, 7);

	// Wait past the force window so everything has fired.
	std::this_thread::sleep_for(500ms);
	deb.finish();

	std::lock_guard<std::mutex> lk(g_deb_mtx);

	int key1_calls = 0, key2_calls = 0, key1_last = -1, key2_val = -1;
	for (auto& [k, v] : g_deb_calls) {
		if (k == 1) { ++key1_calls; key1_last = v; }
		if (k == 2) { ++key2_calls; key2_val = v; }
	}

	// 30 rapid touches of key 1 collapsed: far fewer than 30 calls (the force
	// window may let one through mid-burst, so allow up to a few, not 30).
	assert(key1_calls >= 1 && key1_calls <= 3);
	assert(key1_last == 29);            // the last value always wins
	assert(key2_calls == 1);            // single touch -> single call
	assert(key2_val == 7);

	std::printf("debouncer OK: key1 collapsed 30 touches into %d call(s) (last value %d), key2 fired %d time(s) (value %d)\n",
	            key1_calls, key1_last, key2_calls, key2_val);
}


int main() {
	test_inline_scheduler();
	test_clear_cancels();
	test_threaded_scheduler();
	test_debouncer();
	std::printf("all scheduler tests passed\n");
	return 0;
}
