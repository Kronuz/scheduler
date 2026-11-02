// BaseScheduler::post(): the producer hand-off that lets a thread arm a task with one
// compare-exchange while the worker files it into the wheel.
//
//   1. posted tasks fire in the order posted (one thread, and a chain ordered across threads), and
//      tasks of mixed delays, posted as post() and post_ordered(), fire in due order (the wheel's oracle);
//   2. a cleared task never fires, a due task fires, a distant task wakes a sleeping worker;
//   3. many producers: every accepted task fires exactly once, per-producer order holds;
//   4. racing finish(): a task post() accepted is run, one it refused never is, none is stranded;
//      with a poster held between its `stopping` check and its push, each way the worker can leave
//      (nothing pending, the countdown, finish(0)) closes the stack or runs what it holds;
//   5. nothing runs under the worker's mutex: a cancelled task's destructor that posts, and a task
//      posted between the worker's peep and its sleep;
//   6. posters that read the clock, wait and then push ordered tasks (the logger's shape) keep due order
//      between posts that are ordered in time;
//   7. a pass that finds a task already due walks at once: no mutex, no `sleeping` for producers to signal.
//
// The timing windows are generous; nothing asserts a tight race by luck.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string_view>
#include <thread>
#include <vector>

// Test seams in scheduler.h: the worker's pass (between its peep and its sleep, and where it goes to
// sleep) and a poster between its `stopping` check and its push.
static void pass_hook(const char* site, bool flag);
static void push_hook();
#define SCHEDULER_TEST_HOOK(site, stale) pass_hook(site, stale)
#define SCHEDULER_PUSH_HOOK() push_hook()

#include "scheduler.h"

using namespace std::chrono_literals;

#define CHECK(cond) do { \
	if (!(cond)) { \
		std::fprintf(stderr, "CHECK FAILED at line %d: %s\n", __LINE__, #cond); \
		std::abort(); \
	} \
} while (0)

struct PostTask;
using PostSched = Scheduler<PostTask, ThreadPolicyType::regular>;
using PostBase = ScheduledTask<PostSched, PostTask, ThreadPolicyType::regular>;

struct Sink {
	std::mutex m;
	std::vector<int> fired;   // ids, in firing order
	std::atomic<long> count{0};
};

struct PostTask : public PostBase {
	Sink* sink;
	int id;
	std::chrono::milliseconds hold{0};   // keeps the worker busy this long when it fires
	std::function<void()> on_destroy;    // runs in the destructor, wherever the last reference goes
	std::function<void()> on_fire;       // runs when it fires, on the worker thread
	PostTask(Sink* s, int i) : sink(s), id(i) {}
	~PostTask() {
		if (on_destroy) {
			on_destroy();
		}
	}
	void operator()() {
		{
			std::lock_guard<std::mutex> lk(sink->m);
			sink->fired.push_back(id);
		}
		sink->count.fetch_add(1, std::memory_order_relaxed);
		if (on_fire) {
			on_fire();
		}
		if (hold.count() != 0) {
			std::this_thread::sleep_for(hold);
		}
	}
};

template <class Pred>
static bool wait_for(Pred pred, std::chrono::milliseconds limit = 10s) {
	auto end = std::chrono::steady_clock::now() + limit;
	while (std::chrono::steady_clock::now() < end) {
		if (pred()) {
			return true;
		}
		std::this_thread::sleep_for(1ms);
	}
	return pred();
}

// The worker's pass seam runs `pass_action` once, on the worker thread, when a test arms it. The sleep
// seam counts the times the worker went to sleep (mutex, `sleeping`) although its look found a task due.
static std::atomic<std::function<void()>*> pass_action{nullptr};
static std::atomic<bool> pass_done{false};   // release: the action has finished, and no longer touches the test's locals
static std::atomic<long> slept_while_due{0};
static void pass_hook(const char* site, bool flag) {
	if (std::string_view(site) == "sleep") {
		if (flag) {
			slept_while_due.fetch_add(1, std::memory_order_relaxed);
		}
		return;
	}
	if (auto* f = pass_action.load(std::memory_order_acquire)) {
		if (pass_action.compare_exchange_strong(f, nullptr)) {
			(*f)();
			pass_done.store(true, std::memory_order_release);
		}
	}
}
static void arm_pass(std::function<void()>& action) {
	pass_done.store(false);
	pass_action.store(&action);
}
// Waits for the armed action to finish, so a test may destroy what it captured.
static void await_pass() {
	CHECK(wait_for([] { return pass_done.load(std::memory_order_acquire); }, 5s));
}

// The push seam holds the one poster thread that asked for it (`hold_me`), past its `stopping`
// check and before its compare-exchange, until the test releases it.
static thread_local bool hold_me = false;
static std::mutex hold_mutex;
static std::condition_variable hold_cv;
static bool poster_held = false, poster_release = false;
static void push_hook() {
	if (!hold_me) {
		return;
	}
	std::unique_lock<std::mutex> lk(hold_mutex);
	poster_held = true;
	hold_cv.notify_all();
	hold_cv.wait(lk, [] { return poster_release; });
}

static std::shared_ptr<PostTask> make(Sink& sink, int id) {
	return std::make_shared<PostTask>(&sink, id);
}

static schedule_result post_at(PostSched& s, const std::shared_ptr<PostTask>& t, std::chrono::steady_clock::time_point at) {
	return s.post(t, at, std::chrono::steady_clock::now());
}

// One thread posts tasks due at the same instant and at rising instants; they fire in posting order.
static void test_order() {
	Sink sink;
	PostSched sched("POST");
	std::vector<std::shared_ptr<PostTask>> keep;
	auto base = std::chrono::steady_clock::now() + 5ms;
	constexpr int N = 3000;
	for (int i = 0; i < N; ++i) {
		keep.push_back(make(sink, i));
		// Several tasks per instant, then the next instant: a nondecreasing due time.
		CHECK(post_at(sched, keep.back(), base + std::chrono::microseconds(150 * (i / 7))) == schedule_result::scheduled);
	}
	CHECK(wait_for([&] { return sink.count.load() == N; }));
	std::lock_guard<std::mutex> lk(sink.m);
	for (int i = 0; i < N; ++i) {
		CHECK(sink.fired[i] == i);
	}
	std::printf("post order OK: %d tasks fired in posting order\n", N);
}

// An order established between threads (each posts after seeing the previous one posted)
// is the order they fire in.
static void test_chain_across_threads() {
	Sink sink;
	PostSched sched("POST");
	constexpr int kThreads = 4, kPer = 2000;
	std::atomic<int> turn{0};
	std::vector<std::shared_ptr<PostTask>> keep(kThreads * kPer);
	std::vector<std::thread> threads;
	for (int t = 0; t < kThreads; ++t) {
		threads.emplace_back([&, t] {
			for (int k = 0; k < kPer; ++k) {
				int id = k * kThreads + t;
				while (turn.load(std::memory_order_acquire) != id) {
				}
				keep[id] = make(sink, id);
				CHECK(post_at(sched, keep[id], std::chrono::steady_clock::now()) == schedule_result::scheduled);
				turn.store(id + 1, std::memory_order_release);
			}
		});
	}
	for (auto& th : threads) {
		th.join();
	}
	CHECK(wait_for([&] { return sink.count.load() == kThreads * kPer; }));
	std::lock_guard<std::mutex> lk(sink.m);
	for (int i = 0; i < kThreads * kPer; ++i) {
		CHECK(sink.fired[i] == i);
	}
	std::printf("post chain OK: %d tasks posted in turn by %d threads fired in that order\n", kThreads * kPer, kThreads);
}

static void test_cancel_and_wake() {
	Sink sink;
	PostSched sched("POST");

	// Cleared right after posting: never fires, however the race with the worker goes.
	std::vector<std::shared_ptr<PostTask>> keep;
	for (int i = 0; i < 500; ++i) {
		keep.push_back(make(sink, 1000 + i));
		CHECK(post_at(sched, keep.back(), std::chrono::steady_clock::now() + 2ms) == schedule_result::scheduled);
		keep.back()->clear();
	}
	// A task due soon fires while the worker, idle, sleeps toward its far target.
	std::this_thread::sleep_for(100ms);
	auto soon = make(sink, 1);
	CHECK(post_at(sched, soon, std::chrono::steady_clock::now() + 20ms) == schedule_result::scheduled);
	CHECK(wait_for([&] { return sink.count.load() >= 1; }, 2s));
	// A task due now fires too, and nothing cleared did.
	auto now_task = make(sink, 2);
	CHECK(post_at(sched, now_task, std::chrono::steady_clock::now()) == schedule_result::scheduled);
	CHECK(wait_for([&] { return sink.count.load() >= 2; }, 2s));
	std::this_thread::sleep_for(50ms);
	std::lock_guard<std::mutex> lk(sink.m);
	CHECK(sink.fired.size() == 2);
	CHECK(sink.fired[0] == 1 && sink.fired[1] == 2);
	std::printf("post cancel and wake OK\n");
}


// Tasks of mixed delays that straddle the 1 ms tick, posted the way the logger posts: a delay within the
// tick is raised to exactly one tick and posted ordered, the rest go to the wheel. Oracle: of two tasks
// i < j posted in that order with due_i <= due_j, i fires first. With `stall`, the worker is held in a
// task while they are posted, so it files them all overdue: the wheel then files every one of them at
// the floor of its last walk, and the arrival queue must order its tasks as the wheel would.
static void mixed_order(bool stall) {
	for (int round = 0; round < 20; ++round) {
		Sink sink;
		PostSched sched("POST");
		std::mt19937_64 rng(77 + round);
		constexpr int N = 400;
		constexpr int kBlocker = 100000;
		std::vector<std::shared_ptr<PostTask>> keep;
		std::vector<std::chrono::steady_clock::time_point> due(N);
		std::shared_ptr<PostTask> blocker;
		if (stall) {
			blocker = make(sink, kBlocker);
			blocker->hold = 25ms;
			CHECK(post_at(sched, blocker, std::chrono::steady_clock::now()) == schedule_result::scheduled);
			CHECK(wait_for([&] { return sink.count.load() == 1; }));
			std::this_thread::sleep_for(2ms);   // the worker is now inside the task
		}
		static const int delay_us[] = {0, 0, 0, 100, 300, 500, 950, 1000, 1050, 1500, 2000, 3500};
		for (int i = 0; i < N; ++i) {
			auto now = std::chrono::steady_clock::now();
			auto at = now + std::chrono::microseconds(delay_us[rng() % (sizeof(delay_us) / sizeof(delay_us[0]))]);
			keep.push_back(make(sink, i));
			if (at <= now + 1ms) {
				at = now + 1ms;
				due[i] = at;
				CHECK(sched.post_ordered(keep.back(), at, now) == schedule_result::scheduled);
			} else {
				due[i] = at;
				CHECK(sched.post(keep.back(), at, now) == schedule_result::scheduled);
			}
			if (rng() % 16 == 0) {
				std::this_thread::sleep_for(std::chrono::microseconds(rng() % 400));
			}
		}
		CHECK(wait_for([&] { return sink.count.load() == N + (stall ? 1 : 0); }));
		std::this_thread::sleep_for(10ms);
		std::lock_guard<std::mutex> lk(sink.m);
		CHECK(sink.fired.size() == static_cast<size_t>(N + (stall ? 1 : 0)));
		std::vector<int> position(N, -1);
		for (int k = 0; k < static_cast<int>(sink.fired.size()); ++k) {
			if (sink.fired[k] == kBlocker) {
				continue;
			}
			CHECK(position[sink.fired[k]] == -1);
			position[sink.fired[k]] = k;
		}
		for (int i = 0; i < N; ++i) {
			for (int j = i + 1; j < N; ++j) {
				if (due[i] <= due[j] && position[i] > position[j]) {
					std::fprintf(stderr, "round %d%s: task %d fired after task %d although due first\n", round, stall ? " (stalled)" : "", i, j);
					std::abort();
				}
			}
		}
	}
	std::printf("post mixed order%s OK: 20 rounds of 400 tasks, ordered and wheel, due order held\n", stall ? " with a stalled worker" : "");
}

static void test_mixed_order() {
	mixed_order(false);
	mixed_order(true);
}

// The same across threads, ordered by a turn counter.
static void test_mixed_order_across_threads() {
	Sink sink;
	PostSched sched("POST");
	constexpr int kThreads = 4, kPer = 300;
	constexpr int N = kThreads * kPer;
	std::atomic<int> turn{0};
	std::vector<std::shared_ptr<PostTask>> keep(N);
	std::vector<std::chrono::steady_clock::time_point> due(N);
	std::vector<std::thread> threads;
	for (int t = 0; t < kThreads; ++t) {
		threads.emplace_back([&, t] {
			std::mt19937_64 rng(5 + t);
			static const int delay_us[] = {0, 0, 300, 1000, 1500, 3000};
			for (int k = 0; k < kPer; ++k) {
				int id = k * kThreads + t;
				while (turn.load(std::memory_order_acquire) != id) {
				}
				auto now = std::chrono::steady_clock::now();
				auto at = now + std::chrono::microseconds(delay_us[rng() % (sizeof(delay_us) / sizeof(delay_us[0]))]);
				keep[id] = make(sink, id);
				if (at <= now + 1ms) {
					at = now + 1ms;
					due[id] = at;
					CHECK(sched.post_ordered(keep[id], at, now) == schedule_result::scheduled);
				} else {
					due[id] = at;
					CHECK(sched.post(keep[id], at, now) == schedule_result::scheduled);
				}
				turn.store(id + 1, std::memory_order_release);
			}
		});
	}
	for (auto& th : threads) {
		th.join();
	}
	CHECK(wait_for([&] { return sink.count.load() == N; }));
	std::this_thread::sleep_for(10ms);
	std::lock_guard<std::mutex> lk(sink.m);
	std::vector<int> position(N, -1);
	for (int k = 0; k < N; ++k) {
		position[sink.fired[k]] = k;
	}
	for (int i = 0; i < N; ++i) {
		for (int j = i + 1; j < N; ++j) {
			if (due[i] <= due[j]) {
				CHECK(position[i] < position[j]);
			}
		}
	}
	std::printf("post mixed order across threads OK: %d tasks\n", N);
}

// A distant task is accepted, a task beyond the wheel's span is refused.
static void test_horizon() {
	Sink sink;
	PostSched sched("POST");
	auto far = make(sink, 1);
	CHECK(post_at(sched, far, std::chrono::steady_clock::now() + 23h) == schedule_result::scheduled);
	auto beyond = make(sink, 2);
	CHECK(post_at(sched, beyond, std::chrono::steady_clock::now() + 26h) == schedule_result::past_horizon);
	CHECK(beyond.use_count() == 1);   // a refusal keeps no reference
	std::printf("post horizon OK\n");
}

// Several producers: every accepted task fires once; each producer's tasks fire in its order.
static void test_producers() {
	Sink sink;
	PostSched sched("POST");
	constexpr int kThreads = 4, kPer = 50000;
	std::vector<std::thread> threads;
	std::vector<std::vector<std::shared_ptr<PostTask>>> keep(kThreads);
	for (int t = 0; t < kThreads; ++t) {
		keep[t].reserve(kPer);
		threads.emplace_back([&, t] {
			for (int k = 0; k < kPer; ++k) {
				keep[t].push_back(make(sink, t * kPer + k));
				// Due within 2 ms: a worker slower than the posters (a TSan build) files most of them overdue,
				// all into the leaf at the wheel's floor, whose chain each insert resumes at the furthest node
				// claimed (stash's claim hint; walked from the head, the backlog cost quadratic time).
				CHECK(post_at(sched, keep[t].back(), std::chrono::steady_clock::now() + (k % 2) * 1ms) == schedule_result::scheduled);
			}
		});
	}
	for (auto& th : threads) {
		th.join();
	}
	CHECK(wait_for([&] { return sink.count.load() == kThreads * kPer; }, 30s));
	std::this_thread::sleep_for(20ms);
	std::lock_guard<std::mutex> lk(sink.m);
	CHECK(sink.fired.size() == static_cast<size_t>(kThreads * kPer));
	std::vector<char> seen(kThreads * kPer, 0);
	for (int id : sink.fired) {
		CHECK(!seen[id]);
		seen[id] = 1;
	}
	std::printf("post producers OK: %d tasks from %d threads fired once each\n", kThreads * kPer, kThreads);
}

// finish() racing posters: a task accepted is run (it is due now), one refused is not.
static void test_finish_race() {
	// Half the tasks are post()ed due now, which are overdue by the time the worker files them and land in
	// the wheel's floor slot; 200 rounds of 6000 keep TSan builds quick.
	constexpr int kPerPoster = 2000;
	long accepted_total = 0, fired_total = 0;
	std::chrono::milliseconds slowest{0};
	for (int round = 0; round < 200; ++round) {
		Sink sink;
		std::atomic<long> accepted{0}, refused{0};
		std::atomic<bool> go{false};
		std::vector<std::shared_ptr<PostTask>> kept[3];
		{
			PostSched sched("POST");
			std::vector<std::thread> threads;
			for (int t = 0; t < 3; ++t) {
				threads.emplace_back([&, t] {
					while (!go.load()) {
					}
					for (int k = 0; k < kPerPoster; ++k) {
						kept[t].push_back(make(sink, t * kPerPoster + k));
						auto now = std::chrono::steady_clock::now();
						auto r = (k & 1) ? sched.post(kept[t].back(), now, now) : sched.post_ordered(kept[t].back(), now, now);
						if (r == schedule_result::scheduled) {
							accepted.fetch_add(1);
						} else {
							CHECK(r == schedule_result::stopping);
							refused.fetch_add(1);
							kept[t].back()->clear();   // as the owner would: a refused task is not run
						}
					}
				});
			}
			go.store(true);
			std::this_thread::sleep_for(std::chrono::microseconds(200 + (round % 7) * 300));
			auto began = std::chrono::steady_clock::now();
			bool joined = sched.finish(30);   // joins within 6 s: a starved CI runner once missed 2 s here
			auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began);
			slowest = std::max(slowest, took);
			if (!joined) {
				std::fprintf(stderr, "round %d: finish(30) did not return in %lld ms (accepted %ld, run %ld)\n", round, static_cast<long long>(took.count()), accepted.load(), sink.count.load());
				std::abort();
			}
			for (auto& th : threads) {
				th.join();
			}
		}
		CHECK(sink.count.load() == accepted.load());
		accepted_total += accepted.load();
		fired_total += sink.count.load();
		// Nothing refused was run.
		std::vector<char> seen(3 * kPerPoster, 0);
		for (int id : sink.fired) {
			CHECK(!seen[id]);
			seen[id] = 1;
		}
	}
	std::printf("post finish race OK: %ld accepted tasks over 200 rounds, %ld fired, none stranded (slowest finish %lld ms)\n", accepted_total, fired_total, static_cast<long long>(slowest.count()));
}


// The ways the worker's loop ends while a poster stands between its `stopping` check and its push.
// A task accepted must run (if due) or be released; one refused never runs; none stays on the stack,
// where it holds itself and nothing frees it.
static void finish_exit_case(const char* what, bool far_task, bool zero_wait) {
	Sink sink;
	std::weak_ptr<PostTask> weak;
	schedule_result result{};
	{
		PostSched sched("POST");
		std::this_thread::sleep_for(20ms);
		std::shared_ptr<PostTask> far;
		if (far_task) {
			// Due inside the 100 ms the worker looks ahead while it finishes, so the countdown ends the
			// loop (nothing-pending would close the stack on its own).
			far = make(sink, 2);
			auto now = std::chrono::steady_clock::now();
			CHECK(sched.post(far, now + 60ms, now) == schedule_result::scheduled);
		}
		if (zero_wait) {
			// The worker is inside a task (after the pass's filing) while the task is posted and
			// finish(0) is called: the loop ends at its next test, with the task still on the stack.
			auto blocker = make(sink, 3);
			blocker->hold = 150ms;
			CHECK(post_at(sched, blocker, std::chrono::steady_clock::now()) == schedule_result::scheduled);
			CHECK(wait_for([&] { return sink.count.load() == 1; }));
			std::this_thread::sleep_for(10ms);
			auto t = make(sink, 1);
			weak = t;
			result = post_at(sched, t, std::chrono::steady_clock::now());
			t.reset();
			sched.finish(0);
			CHECK(wait_for([&] { return sink.count.load() >= 2 || result != schedule_result::scheduled; }, 5s));
		} else {
			poster_held = poster_release = false;
			std::thread poster([&] {
				hold_me = true;
				auto t = make(sink, 1);
				weak = t;
				result = post_at(sched, t, std::chrono::steady_clock::now());
			});
			{
				std::unique_lock<std::mutex> lk(hold_mutex);
				hold_cv.wait(lk, [] { return poster_held; });
			}
			// The worker leaves while the poster is held (finish(1) joins within 200 ms; a loaded host may take longer).
			if (!sched.finish(1)) {
				CHECK(sched.join(10s));
			}
			{
				std::lock_guard<std::mutex> lk(hold_mutex);
				poster_release = true;
			}
			hold_cv.notify_all();
			poster.join();
		}
		std::this_thread::sleep_for(50ms);
		std::lock_guard<std::mutex> lk(sink.m);
		bool ran = false;
		for (int id : sink.fired) {
			ran = ran || id == 1;
		}
		if (result == schedule_result::scheduled) {
			if (!ran) {
				std::fprintf(stderr, "%s: a task post() accepted never ran (and is %s)\n", what, weak.expired() ? "freed" : "still held");
				std::abort();
			}
		} else {
			CHECK(result == schedule_result::stopping);
			CHECK(!ran);
		}
	}
	CHECK(weak.expired());   // the scheduler is gone: nothing holds the task, whichever way it went
}

static void test_finish_exit_nothing_pending() {
	finish_exit_case("nothing pending", false, false);
	std::printf("post finish exit OK: nothing pending closes the stack\n");
}

static void test_finish_exit_countdown() {
	finish_exit_case("countdown", true, false);
	std::printf("post finish exit OK: the countdown exit closes the stack\n");
}

static void test_finish_exit_zero() {
	finish_exit_case("finish(0)", false, true);
	std::printf("post finish exit OK: finish(0) runs the task it left on the stack\n");
}

// A task cleared before the worker files it is dropped when it is filed; its destructor posts.
// Filing must not happen under the worker's mutex: post() takes it to signal.
static void test_cancelled_dtor_posts() {
	Sink sink;
	PostSched sched("POST");
	std::this_thread::sleep_for(30ms);   // the worker is asleep on its idle target
	auto follower = make(sink, 2);
	auto cancelled = make(sink, 1);
	cancelled->on_destroy = [&] {
		auto now = std::chrono::steady_clock::now();
		sched.post(follower, now, now);
	};
	cancelled->clear();
	std::shared_ptr<PostTask> held = cancelled;
	cancelled.reset();
	std::function<void()> action = [&] {
		// On the worker, between its peep and its sleep: post the cancelled task and drop our reference.
		auto now = std::chrono::steady_clock::now();
		sched.post(held, now, now);
		held.reset();
	};
	arm_pass(action);
	auto wake = make(sink, 3);
	CHECK(post_at(sched, wake, std::chrono::steady_clock::now()) == schedule_result::scheduled);   // a pass
	if (!wait_for([&] { return sink.count.load() >= 2; }, 5s)) {
		std::fprintf(stderr, "the worker stopped: a cancelled task's destructor that posts hung it\n");
		std::_Exit(1);
	}
	await_pass();
	std::printf("post cancelled destructor OK: a destructor that posts does not hang the worker\n");
}

// A task posted between the worker's peep and its sleep is run now, not when the worker's idle
// target (30 s) is up.
static void test_post_between_peep_and_sleep() {
	for (int ordered = 0; ordered < 2; ++ordered) {
		Sink sink;
		PostSched sched("POST");
		std::this_thread::sleep_for(30ms);
		auto t = make(sink, 1);
		std::function<void()> action = [&] {
			auto now = std::chrono::steady_clock::now();
			if (ordered) {
				sched.post_ordered(t, now + 1ms, now);
			} else {
				sched.post(t, now, now);
			}
		};
		arm_pass(action);
		auto wake = make(sink, 2);
		CHECK(post_at(sched, wake, std::chrono::steady_clock::now()) == schedule_result::scheduled);
		CHECK(wait_for([&] { return sink.count.load() == 2; }, 2s));
		await_pass();
	}
	std::printf("post between peep and sleep OK\n");
}

// The worker has woken by timeout, so the published wakeup time is already past and a poster has nothing to
// lower; a task due now is posted between the worker's peep and its sleep. The poster sees `sleeping` false and
// sends no signal, and the worker then raises the published time to its idle target (30 s): only its look at
// the stack, after it stores `sleeping`, saves the task from waiting that out.
static void test_post_between_peep_and_sleep_stale() {
	Sink sink;
	PostSched sched("POST");
	std::this_thread::sleep_for(30ms);
	auto first = make(sink, 1);
	auto second = make(sink, 2);
	std::atomic<bool> second_posted{false};
	std::function<void()> action = [&] {
		if (sink.count.load() < 1) {   // the first task has not fired yet: try again on the next pass
			pass_action.store(&action);
			return;
		}
		auto t = std::chrono::steady_clock::now();
		sched.post(second, t, t);
		second_posted.store(true, std::memory_order_release);
	};
	arm_pass(action);
	auto now = std::chrono::steady_clock::now();
	// Due in 40 ms: the worker sleeps to that time and wakes by its timeout (the first pass after it runs
	// the task is the one in which the action posts the second).
	CHECK(sched.post(first, now + 40ms, now) == schedule_result::scheduled);
	if (!wait_for([&] { return sink.count.load() == 2; }, 3s)) {
		std::fprintf(stderr, "a task posted between the worker's peep and its sleep, with nothing for post() to lower, waited out the idle target\n");
		std::_Exit(1);
	}
	CHECK(wait_for([&] { return second_posted.load(std::memory_order_acquire); }, 2s));
	std::printf("post between peep and sleep (stale target) OK\n");
}

// Posters that read the clock, wait a random time, and push (the logger's shape: its wakeup is the
// clock plus a fixed delay). The times fall out of order across posters, by the wait. Of two posts
// where one was pushed before the other read its clock, the first fires first when it is due no later.
static void test_ordered_posters() {
	for (int round = 0; round < 5; ++round) {
		Sink sink;
		PostSched sched("POST");
		constexpr int kThreads = 4, kPer = 500, N = kThreads * kPer;
		std::vector<std::shared_ptr<PostTask>> keep(N);
		std::vector<std::chrono::steady_clock::time_point> started(N), pushed(N), due(N);
		std::vector<std::thread> threads;
		for (int t = 0; t < kThreads; ++t) {
			threads.emplace_back([&, t] {
				std::mt19937_64 rng(1000 * round + t);
				static const int delay_us[] = {0, 0, 300, 1000, 1500, 3000};
				for (int k = 0; k < kPer; ++k) {
					int id = t * kPer + k;
					keep[id] = make(sink, id);
					auto now = std::chrono::steady_clock::now();
					auto at = now + std::chrono::microseconds(delay_us[rng() % (sizeof(delay_us) / sizeof(delay_us[0]))]);
					bool ordered = at <= now + 1ms;
					if (ordered) {
						at = now + 1ms;
					}
					started[id] = now;
					due[id] = at;
					auto spin_until = std::chrono::steady_clock::now() + std::chrono::microseconds(rng() % 150);
					while (std::chrono::steady_clock::now() < spin_until) {
					}
					CHECK((ordered ? sched.post_ordered(keep[id], at, now) : sched.post(keep[id], at, now)) == schedule_result::scheduled);
					pushed[id] = std::chrono::steady_clock::now();
					if (rng() % 8 == 0) {
						std::this_thread::sleep_for(std::chrono::microseconds(rng() % 300));
					}
				}
			});
		}
		for (auto& th : threads) {
			th.join();
		}
		CHECK(wait_for([&] { return sink.count.load() == N; }));
		std::this_thread::sleep_for(10ms);
		std::lock_guard<std::mutex> lk(sink.m);
		std::vector<int> position(N, -1);
		for (int k = 0; k < N; ++k) {
			CHECK(position[sink.fired[k]] == -1);
			position[sink.fired[k]] = k;
		}
		long pairs = 0;
		for (int a = 0; a < N; ++a) {
			for (int b = 0; b < N; ++b) {
				if (a != b && pushed[a] < started[b] && due[a] <= due[b]) {
					++pairs;
					if (position[a] > position[b]) {
						std::fprintf(stderr, "round %d: task %d (pushed before task %d read its clock, due no later) fired after it\n", round, a, b);
						std::abort();
					}
				}
			}
		}
		CHECK(pairs > 0);
	}
	std::printf("post ordered posters OK: 5 rounds of 4 posters that wait between the clock and the push\n");
}

// A pass whose look finds a task already due walks at once. It takes no mutex and never sets `sleeping`, so
// a producer arming tasks meanwhile has no sleeping worker to signal and does not queue on the mutex the
// worker holds (with the worker taking it on every pass, one producer's add() cost 1.33 times as much).
// Each task posts the next one, due now: the worker files it at the top of its next pass, whose look then
// finds it due.
static void test_busy_pass_takes_no_lock() {
	Sink sink;
	PostSched sched("POST");
	constexpr int kChain = 50;
	std::vector<std::shared_ptr<PostTask>> chain;
	for (int i = 0; i < kChain; ++i) {
		chain.push_back(make(sink, i));
	}
	for (int i = 0; i + 1 < kChain; ++i) {
		chain[i]->on_fire = [&sched, next = chain[i + 1]] {
			auto now = std::chrono::steady_clock::now();
			CHECK(sched.post(next, now, now) == schedule_result::scheduled);
		};
	}
	slept_while_due.store(0);
	auto now = std::chrono::steady_clock::now();
	CHECK(sched.post(chain[0], now, now) == schedule_result::scheduled);
	CHECK(wait_for([&] { return sink.count.load() == kChain; }, 5s));
	if (slept_while_due.load() != 0) {
		std::fprintf(stderr, "the worker took the mutex and announced `sleeping` %ld times with a task already due\n", slept_while_due.load());
		std::abort();
	}
	std::printf("post busy pass OK: %d passes that found a task due walked without the mutex\n", kChain - 1);
}

int main(int argc, char** argv) {
	struct Entry {
		const char* name;
		void (*run)();
	};
	static const Entry tests[] = {
		{"order", test_order},
		{"chain", test_chain_across_threads},
		{"mixed", test_mixed_order},
		{"mixed_threads", test_mixed_order_across_threads},
		{"cancel", test_cancel_and_wake},
		{"horizon", test_horizon},
		{"producers", test_producers},
		{"finish_race", test_finish_race},
		{"finish_exit_none", test_finish_exit_nothing_pending},
		{"finish_exit_countdown", test_finish_exit_countdown},
		{"finish_exit_zero", test_finish_exit_zero},
		{"dtor_posts", test_cancelled_dtor_posts},
		{"peep_sleep", test_post_between_peep_and_sleep},
		{"peep_sleep_stale", test_post_between_peep_and_sleep_stale},
		{"ordered_posters", test_ordered_posters},
		{"busy_pass", test_busy_pass_takes_no_lock},
	};
	// With arguments, run only the named tests (to run one on its own).
	for (const auto& t : tests) {
		bool wanted = argc == 1;
		for (int i = 1; i < argc; ++i) {
			wanted = wanted || std::string_view(argv[i]) == t.name;
		}
		if (wanted) {
			t.run();
		}
	}
	std::printf("all post tests passed\n");
	return 0;
}
