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
// 1c. Scheduler: add() reports a refusal instead of dropping silently. A task
// past the 24 h horizon returns false and never fires; so does an add during
// shutdown; a normal add returns true and fires.
// ---------------------------------------------------------------------------

static void test_add_result() {
	g_inline_ran.store(0);
	{
		std::lock_guard<std::mutex> lk(g_order_mtx);
		g_order.clear();
	}
	InlineSched sched("SCHED3");
	static_assert(noexcept(sched.add(std::shared_ptr<InlineTaskBase>{}, std::chrono::steady_clock::now())));

	auto now = std::chrono::steady_clock::now();
	auto result = sched.add(std::make_shared<OrderTask>(-1), now + 48h);
	assert(result == schedule_result::past_horizon && "past-horizon add accepted");
	result = sched.add(std::make_shared<OrderTask>(7), now + 50ms);
	assert(result == schedule_result::scheduled && "normal add refused");
	for (int i = 0; i < 100 && g_inline_ran.load() < 1; ++i) {
		std::this_thread::sleep_for(20ms);
	}
	sched.finish();
	result = sched.add(std::make_shared<OrderTask>(-2), std::chrono::steady_clock::now() + 10ms);
	assert(result == schedule_result::stopping && "add during shutdown accepted");
	std::this_thread::sleep_for(50ms);

	std::lock_guard<std::mutex> lk(g_order_mtx);
	assert(g_inline_ran.load() == 1 && g_order.size() == 1 && g_order[0] == 7 && "a refused task fired");
	(void)result;
	std::printf("scheduler OK: add() returns scheduled, past_horizon, stopping\n");
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
// 2b. ThreadedScheduler: a task cancelled AFTER it was dispatched to the worker
//     pool, but BEFORE it runs, must NOT fire (regression for the cancelled-
//     after-dispatch UAF/contract bug).
//
//     The race is hard to hit by luck, so we force the window: a single-worker
//     pool is occupied by a blocker task that parks on an atomic gate. While the
//     blocker holds the only worker, the target task is dispatched by the wheel
//     and sits queued behind it. We cancel the target, then release the blocker;
//     when the worker finally dequeues the target, the execution-time re-check
//     must suppress it.
// ---------------------------------------------------------------------------

struct GateTask;
using GateSched = ThreadedScheduler<GateTask, ThreadPolicyType::regular>;
using GateTaskBase = ScheduledTask<GateSched, GateTask, ThreadPolicyType::regular>;

static std::atomic<bool> g_blocker_release{false};
static std::atomic<int>  g_blocker_ran{0};
static std::atomic<int>  g_target_ran{0};

struct GateTask : public GateTaskBase {
	bool is_blocker;
	explicit GateTask(bool is_blocker) : is_blocker(is_blocker) {}
	void operator()() {
		if (is_blocker) {
			g_blocker_ran.fetch_add(1, std::memory_order_relaxed);
			// Hold the single worker until the test releases us.
			while (!g_blocker_release.load(std::memory_order_acquire)) {
				std::this_thread::sleep_for(5ms);
			}
		} else {
			g_target_ran.fetch_add(1, std::memory_order_relaxed);
		}
	}
};

static void test_cancel_after_dispatch() {
	g_blocker_release.store(false);
	g_blocker_ran.store(0);
	g_target_ran.store(0);

	// One worker only, so the blocker monopolizes the pool.
	GateSched sched("CSCHED", "CW{:02}", 1);

	auto now = std::chrono::steady_clock::now();

	auto blocker = std::make_shared<GateTask>(true);
	auto target  = std::make_shared<GateTask>(false);

	// Blocker fires first and grabs the only worker; target fires shortly after
	// and is dispatched (enqueued) but cannot run while the blocker holds it.
	sched.add(blocker, now + 100ms);
	sched.add(target,  now + 200ms);

	// Wait until the blocker is actually running (worker is occupied).
	for (int i = 0; i < 100 && g_blocker_ran.load() == 0; ++i) {
		std::this_thread::sleep_for(10ms);
	}
	assert(g_blocker_ran.load() == 1);

	// Give the wheel time to pass the target's wakeup time and dispatch it into
	// the (now busy) pool queue.
	std::this_thread::sleep_for(300ms);

	// Cancel the target while it sits queued behind the blocker. The wheel has
	// already claimed the one-shot, so this clear() loses the time-CAS (returns
	// false) but still records cancellation.
	target->clear();

	// Release the blocker; the worker dequeues the target next and must skip it.
	g_blocker_release.store(true, std::memory_order_release);

	std::this_thread::sleep_for(200ms);
	sched.finish();

	assert(g_target_ran.load() == 0);   // cancelled-after-dispatch task never ran
	std::printf("threaded scheduler OK: task cancelled after dispatch did not fire\n");
}


// ---------------------------------------------------------------------------
// 2c. Debouncer: destructs cleanly at scope exit WITHOUT a manual finish(),
//     while debounced calls are in flight (regression for the shutdown UAF).
//
//     The debounced callback touches a heap-allocated counter through a
//     captured pointer; if the derived Debouncer members were torn down before
//     the worker/scheduler threads were joined, an in-flight task would read
//     freed state. A clean run (especially under ASan) exercises the fix.
// ---------------------------------------------------------------------------

static void test_debouncer_destructor_teardown() {
	// Lives independently of the Debouncer so a late callback (one that ran AFTER
	// the Debouncer's members should have been torn down) is still observable here.
	std::atomic<int> ran{0};

	{
		auto deb = make_debouncer<int>(
			"DEBT", "DT{:02}", 2,
			[&ran](int /*key*/, int /*value*/) {
				ran.fetch_add(1, std::memory_order_relaxed);
				// A little work so a call is plausibly still in flight at scope
				// exit, exercising the teardown ordering: the in-flight task
				// touches debouncer.func / debouncer.throttle(key) / statuses,
				// which is exactly the freed-memory read the destructor prevents.
				std::this_thread::sleep_for(5ms);
			},
			/*throttle_time*/            0ms,
			/*debounce_timeout*/         20ms,
			/*debounce_busy_timeout*/    20ms,
			/*debounce_min_force*/       40ms,
			/*debounce_max_force*/       80ms);

		// Fire a burst across several keys so tasks are scheduled and some are
		// dispatched to the worker pool right around the time we leave scope.
		for (int k = 0; k < 8; ++k) {
			for (int v = 0; v < 5; ++v) {
				deb.debounce(k, k, v);
			}
		}

		// Leave only a short pause: we WANT to destruct (scope exit below) with
		// work still potentially in flight, WITHOUT calling finish() ourselves.
		std::this_thread::sleep_for(30ms);
	}   // <-- ~Debouncer runs here; it must join the wheel + worker threads
	    //     (and thus drain any in-flight callback) BEFORE the derived
	    //     members (func, statuses, ...) are destroyed.

	// The destructor has returned. Because it joined the threads before tearing
	// down members, NO callback may run from here on. Snapshot, wait, re-check:
	// a non-zero delta would mean a task outlived the object (the UAF window).
	int after_dtor = ran.load();
	std::this_thread::sleep_for(100ms);
	assert(ran.load() == after_dtor);   // no callback fired after destruction

	std::printf("debouncer OK: clean destructor teardown without manual finish() "
	            "(callbacks ran: %d, none after destruction)\n", after_dtor);
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
	const auto burst_start = std::chrono::steady_clock::now();
	for (int v = 0; v < 30; ++v) {
		deb.debounce(1, 1, v);
		std::this_thread::sleep_for(2ms);   // fast enough to keep pushing it out
	}
	const auto burst = std::chrono::steady_clock::now() - burst_start;

	// Touch key 2 once.
	deb.debounce(2, 2, 7);

	// Wait until both keys' last values fired, however slow the machine.
	for (auto until = std::chrono::steady_clock::now() + 5s; std::chrono::steady_clock::now() < until;) {
		{
			std::lock_guard<std::mutex> lk(g_deb_mtx);
			bool last1 = false, last2 = false;
			for (auto& [k, v] : g_deb_calls) { last1 |= k == 1 && v == 29; last2 |= k == 2 && v == 7; }
			if (last1 && last2) { break; }
		}
		std::this_thread::sleep_for(10ms);
	}
	deb.finish();

	std::lock_guard<std::mutex> lk(g_deb_mtx);

	int key1_calls = 0, key2_calls = 0, key1_last = -1, key2_val = -1;
	for (auto& [k, v] : g_deb_calls) {
		if (k == 1) { ++key1_calls; key1_last = v; }
		if (k == 2) { ++key2_calls; key2_val = v; }
	}

	// 30 rapid touches of key 1 collapsed: the force window lets at most one call through per
	// minimum force interval of the burst, plus the final one; a slow machine stretches the
	// burst, so the bound follows its measured length.
	const int forced = static_cast<int>(burst / 150ms);
	assert(key1_calls >= 1 && key1_calls <= 2 + forced);
	assert(key1_last == 29);            // the last value always wins
	assert(key2_calls == 1);            // single touch -> single call
	assert(key2_val == 7);

	std::printf("debouncer OK: key1 collapsed 30 touches into %d call(s) (last value %d), key2 fired %d time(s) (value %d)\n",
	            key1_calls, key1_last, key2_calls, key2_val);
}

// A touch landing while the pending call is pinned at the force deadline still delivers its
// value: touches every 10ms push out by 50ms until clamped at 150ms, where the last ones land.
static void test_debouncer_force_window() {
	std::mutex mtx;
	std::vector<int> calls;
	auto deb = make_debouncer<int>(
		"DFW", "DF{:02}", 2,
		[&](int, int value) {
			std::lock_guard<std::mutex> lk(mtx);
			calls.push_back(value);
		},
		0ms, 50ms, 50ms, 150ms, 150ms);
	const auto start = std::chrono::steady_clock::now();
	int last = 0;
	for (int v = 0; std::chrono::steady_clock::now() - start < 140ms; ++v) {
		deb.debounce(1, 1, v);
		last = v;
		std::this_thread::sleep_for(10ms);
	}
	for (auto until = std::chrono::steady_clock::now() + 5s; std::chrono::steady_clock::now() < until;) {
		{
			std::lock_guard<std::mutex> lk(mtx);
			if (!calls.empty() && calls.back() == last) { break; }
		}
		std::this_thread::sleep_for(10ms);
	}
	deb.finish();
	std::lock_guard<std::mutex> lk(mtx);
	assert(!calls.empty() && calls.back() == last);
	std::printf("debouncer force window OK: %zu call(s), last value %d delivered\n", calls.size(), last);
}

// Touches on a key whose call is pending re-arm it in place instead of cancelling
// and re-adding: the arguments of a thousand touches are never alive at once, and
// the call still fires once, with the last arguments.
static std::atomic<int> g_counted_alive{0};
struct Counted {
	int value;
	explicit Counted(int v = 0) : value(v) { g_counted_alive.fetch_add(1); }
	Counted(const Counted& o) : value(o.value) { g_counted_alive.fetch_add(1); }
	Counted& operator=(const Counted&) = default;
	~Counted() { g_counted_alive.fetch_sub(1); }
};

static void test_debouncer_rearm() {
	std::mutex mtx;
	std::vector<int> calls;
	auto deb = make_debouncer<int>(
		"DRA", "DR{:02}", 2,
		[&](int, Counted c) {
			std::lock_guard<std::mutex> lk(mtx);
			calls.push_back(c.value);
		},
		0ms, 100ms, 100ms, 2s, 2s);
	for (int v = 0; v < 1000; ++v) {
		deb.debounce(1, 1, Counted(v));
	}
	assert(g_counted_alive.load() <= 2 && "pending touches piled up");
	for (auto until = std::chrono::steady_clock::now() + 5s; std::chrono::steady_clock::now() < until;) {
		{
			std::lock_guard<std::mutex> lk(mtx);
			if (!calls.empty()) { break; }
		}
		std::this_thread::sleep_for(10ms);
	}
	std::this_thread::sleep_for(200ms);
	deb.finish();
	std::lock_guard<std::mutex> lk(mtx);
	assert(calls.size() == 1 && calls[0] == 999);
	std::printf("debouncer re-arm OK: 1000 touches held %d argument set(s), 1 call with the last value\n", 1);
}

// debounce() reports the scheduler's result, and a refused touch leaves nothing
// pending. A touch that would move a pending call past the 24 h horizon is
// refused (past_horizon) and leaves that call as it was, so it still fires with
// its earlier arguments. Force timeouts of 48 h let a touch reach that far.
static void test_debouncer_refusals() {
	std::mutex mtx;
	std::vector<int> calls;
	auto deb = make_debouncer<int>(
		"DRF", "DF{:02}", 2,
		[&](int, int value) {
			std::lock_guard<std::mutex> lk(mtx);
			calls.push_back(value);
		},
		0ms, 50ms, 50ms, 48h, 48h);
	static_assert(noexcept(deb.debounce(1, 1, 0)));

	auto result = deb.delayed_debounce(25h, 1, 1, 1);
	assert(result == schedule_result::past_horizon && "touch past the horizon accepted");
	assert(deb.pending_keys() == 0 && "a refused touch left the key pending");

	result = deb.debounce(2, 2, 10);
	assert(result == schedule_result::scheduled);
	result = deb.delayed_debounce(25h, 2, 2, 11);   // would move the pending call past 24 h
	assert(result == schedule_result::past_horizon && "a touch past the horizon moved a pending call");
	// The pending call keeps its target (~50 ms) and its arguments.
	for (auto until = std::chrono::steady_clock::now() + 5s; deb.pending_keys() != 0 && std::chrono::steady_clock::now() < until;) {
		std::this_thread::sleep_for(10ms);
	}
	std::this_thread::sleep_for(100ms);
	{
		std::lock_guard<std::mutex> lk(mtx);
		assert(calls.size() == 1 && calls[0] == 10 && "a touch past the horizon dropped the earlier call");
	}
	assert(deb.pending_keys() == 0);

	deb.finish();
	result = deb.debounce(3, 3, 30);
	assert(result == schedule_result::stopping && "touch during shutdown accepted");
	assert(deb.pending_keys() == 0 && "a touch refused at shutdown left the key pending");
	std::this_thread::sleep_for(100ms);
	std::lock_guard<std::mutex> lk(mtx);
	assert(calls.size() == 1 && "a refused touch ran a call");
	(void)result;
	std::printf("debouncer refusals OK: past_horizon and stopping leave nothing pending; a past-horizon touch keeps the pending call\n");
}

// After finish(), a touch on a key whose call is still pending returns stopping
// and releases the key: the stopping pool may never run that call, so the
// caller must (the reviewer's debouncer_probe.cc shutdown case).
static void test_debouncer_touch_after_finish() {
	std::atomic<int> ran{0};
	auto deb = make_debouncer<int>("DTF", "DT{:02}", 2,
		[&](int, int) { ran.fetch_add(1); }, 0ms, 200ms, 200ms, 1s, 1s);
	auto before = deb.debounce(1, 1, 10);   // pending, due at ~200 ms
	deb.finish();
	auto pending_after = deb.debounce(1, 1, 11);
	auto fresh_after = deb.debounce(2, 2, 20);
	std::this_thread::sleep_for(600ms);
	assert(before == schedule_result::scheduled);
	assert(!(pending_after == schedule_result::scheduled && ran.load() == 0) && "a touch after finish() was promised a call nothing runs");
	assert(pending_after == schedule_result::stopping && fresh_after == schedule_result::stopping);
	assert(deb.pending_keys() == 0 && "a key stayed pending after finish()");
	(void)before;
	(void)pending_after;
	(void)fresh_after;
	std::printf("debouncer OK: a touch after finish() on a pending key returns stopping\n");
}

int main() {
	test_inline_scheduler();
	test_clear_cancels();
	test_add_result();
	test_threaded_scheduler();
	test_cancel_after_dispatch();
	test_debouncer();
	test_debouncer_force_window();
	test_debouncer_rearm();
	test_debouncer_refusals();
	test_debouncer_touch_after_finish();
	test_debouncer_destructor_teardown();
	std::printf("all scheduler tests passed\n");
	return 0;
}
