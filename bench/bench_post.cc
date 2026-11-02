// What a producer pays to arm a task: add() (the wheel insert on the caller's thread), post() (a hand-off
// the worker files into the wheel) and post_ordered() (a hand-off the worker keeps in an arrival queue),
// each for tasks due 1 ms after the call, with the worker running them meanwhile. The task is made in the
// loop in every case, so the "make only" row is the part they share.
//
// With --compare it also times the timer queue a caller writes without the library: a std::priority_queue
// of (due time, arrival number, task) under one mutex, and one thread that sleeps on a condition variable
// until the earliest task is due, takes the due tasks out in batches of at most 64 and runs them with the
// lock released. It holds the same shared_ptr<Task> the wheel is given and calls it directly. The "queue"
// row is its arming cost, measured as add()'s is; the fire rows time the running: 200,000 tasks due at one
// instant, the time from the first run to the last, per task.
//
// --check (it also runs first under --compare) is the same-results check: tasks armed in shuffled order,
// three to a due time, fire from the wheel and from the queue in the same order, due time first and arming
// order within it.
//
// Build: cmake -B build -DSCHEDULER_BENCH=ON && cmake --build build --target scheduler_bench_post
// Run:   scheduler_bench_post [--compare] [--check]; time it under `quiet strict`.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <queue>
#include <random>
#include <thread>
#include <vector>

#include "scheduler.h"

using Clock = std::chrono::steady_clock;

// The comparator: a binary heap of due tasks under a mutex, run by one thread.
template <class Task>
class HeapTimers {
public:
	HeapTimers() : thread_([this] { loop(); }) {}
	HeapTimers(const HeapTimers&) = delete;
	HeapTimers& operator=(const HeapTimers&) = delete;
	~HeapTimers() {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			stopping_ = true;
		}
		wake_.notify_one();
		thread_.join();
	}

	void add(std::shared_ptr<Task> task, Clock::time_point at) {
		bool earliest;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			earliest = heap_.empty() || at < heap_.top().at;
			heap_.push(Entry{at, next_++, std::move(task)});
		}
		if (earliest) {
			wake_.notify_one();
		}
	}

private:
	struct Entry {
		Clock::time_point at;
		std::uint64_t arrival;
		std::shared_ptr<Task> task;
		// std::priority_queue is a max-heap: the earliest due time, then the earliest arrival, is the top.
		bool operator<(const Entry& other) const {
			return at != other.at ? at > other.at : arrival > other.arrival;
		}
	};

	void loop() {
		std::vector<std::shared_ptr<Task>> due;
		std::unique_lock<std::mutex> lock(mutex_);
		for (;;) {
			if (stopping_) {
				return;
			}
			if (heap_.empty()) {
				wake_.wait(lock);
				continue;
			}
			auto now = Clock::now();
			if (heap_.top().at > now) {
				wake_.wait_until(lock, heap_.top().at);
				continue;
			}
			due.clear();
			while (!heap_.empty() && heap_.top().at <= now && due.size() < 64) {
				due.push_back(std::move(const_cast<Entry&>(heap_.top()).task));  // the key stays; only the pointer moves
				heap_.pop();
			}
			lock.unlock();
			for (auto& task : due) {
				(*task)();
			}
			lock.lock();
		}
	}

	std::mutex mutex_;
	std::condition_variable wake_;
	std::priority_queue<Entry> heap_;
	std::uint64_t next_ = 0;
	bool stopping_ = false;
	std::thread thread_;  // last: started once the rest exists
};

struct Trivial;
using BenchSched = Scheduler<Trivial, ThreadPolicyType::regular>;
using BenchBase = ScheduledTask<BenchSched, Trivial, ThreadPolicyType::regular>;

// The fire rows stamp the first run and the run numbered `mark` (the last one); no other row sets it.
static std::atomic<long> ran;
static std::atomic<long> mark{-1};
static std::atomic<bool> finished{false};
static Clock::time_point stamps[2];
struct Trivial : BenchBase {
	void operator()() {
		long n = ran.fetch_add(1, std::memory_order_relaxed);
		if (n == 0 || n == mark.load(std::memory_order_relaxed)) {
			stamps[n == 0 ? 0 : 1] = Clock::now();
			if (n != 0) {
				finished.store(true, std::memory_order_release);
			}
		}
	}
};

static double now_s() {
	return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

enum class Mode { make_only, add, post, post_ordered, queue };

static const char* name(Mode m) {
	switch (m) {
		case Mode::make_only: return "make only";
		case Mode::add: return "add()";
		case Mode::post: return "post()";
		case Mode::queue: return "queue";
		default: return "post_ordered()";
	}
}

static double run(Mode mode, int threads, long per_thread) {
	std::optional<BenchSched> sched;
	std::optional<HeapTimers<Trivial>> timers;
	if (mode == Mode::queue) {
		timers.emplace();
	} else {
		sched.emplace("BENCH");
	}
	ran = 0;
	std::atomic<long long> total_ns{0};
	std::vector<std::thread> ts;
	for (int t = 0; t < threads; ++t) {
		ts.emplace_back([&] {
			std::vector<std::shared_ptr<Trivial>> keep;
			keep.reserve(static_cast<size_t>(per_thread));
			double t0 = now_s();
			for (long i = 0; i < per_thread; ++i) {
				auto task = std::make_shared<Trivial>();
				auto now = Clock::now();
				auto at = now + std::chrono::milliseconds(1);
				switch (mode) {
					case Mode::make_only: break;
					case Mode::add: sched->add(task, at); break;
					case Mode::post: sched->post(task, at, now); break;
					case Mode::post_ordered: sched->post_ordered(task, at, now); break;
					case Mode::queue: timers->add(task, at); break;
				}
				keep.push_back(std::move(task));
			}
			total_ns += static_cast<long long>((now_s() - t0) * 1e9);
		});
	}
	for (auto& t : ts) {
		t.join();
	}
	long want = mode == Mode::make_only ? 0 : per_thread * threads;
	auto end = Clock::now() + std::chrono::seconds(30);
	while (ran.load() < want && Clock::now() < end) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return static_cast<double>(total_ns) / static_cast<double>(per_thread * threads);
}

// `n` tasks due at one instant, armed by one thread well before it: the time from the first task's run to
// the last one's, per task. Negative when they did not all run in time or were armed too late.
static double fire(bool wheel, long n) {
	std::optional<BenchSched> sched;
	std::optional<HeapTimers<Trivial>> timers;
	if (wheel) {
		sched.emplace("BENCH");
	} else {
		timers.emplace();
	}
	ran = 0;
	finished = false;
	mark = n - 1;
	std::vector<std::shared_ptr<Trivial>> keep;
	keep.reserve(static_cast<size_t>(n));
	auto due = Clock::now() + std::chrono::milliseconds(500);
	for (long i = 0; i < n; ++i) {
		auto task = std::make_shared<Trivial>();
		if (wheel) {
			sched->add(task, due);
		} else {
			timers->add(task, due);
		}
		keep.push_back(std::move(task));
	}
	bool in_time = Clock::now() < due;
	auto end = Clock::now() + std::chrono::seconds(30);
	while (!finished.load(std::memory_order_acquire) && Clock::now() < end) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	bool done = finished.load(std::memory_order_acquire);
	mark = -1;
	if (!in_time || !done) {
		return -1;
	}
	return std::chrono::duration<double, std::nano>(stamps[1] - stamps[0]).count() / static_cast<double>(n - 1);
}

// The same-results check.
struct Recorded;
using RecordedSched = Scheduler<Recorded, ThreadPolicyType::regular>;
static std::mutex order_mutex;
static std::vector<int> order;
struct Recorded : ScheduledTask<RecordedSched, Recorded, ThreadPolicyType::regular> {
	int id;
	explicit Recorded(int id) : id(id) {}
	void operator()() {
		std::lock_guard<std::mutex> lock(order_mutex);
		order.push_back(id);
	}
};

static bool wait_for(std::size_t count) {
	auto end = Clock::now() + std::chrono::seconds(10);
	for (;;) {
		{
			std::lock_guard<std::mutex> lock(order_mutex);
			if (order.size() >= count) {
				return true;
			}
		}
		if (Clock::now() >= end) {
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

static bool check() {
	constexpr int tasks = 300;
	constexpr int per_time = 3;
	std::mt19937 random(7);
	std::vector<int> arming(tasks);
	std::iota(arming.begin(), arming.end(), 0);
	std::shuffle(arming.begin(), arming.end(), random);
	// Task `id` is due in slot id / per_time, slots 2 ms apart (past the wheel's 1 ms tick); the tasks of
	// a slot share one time point. Due time first, arming order within it.
	std::vector<int> expected(arming);
	std::stable_sort(expected.begin(), expected.end(), [](int a, int b) { return a / per_time < b / per_time; });
	auto fired = [&](auto&& arm) {
		{
			std::lock_guard<std::mutex> lock(order_mutex);
			order.clear();
		}
		auto base = Clock::now() + std::chrono::milliseconds(100);
		std::vector<std::shared_ptr<Recorded>> keep;
		for (int id : arming) {
			auto task = std::make_shared<Recorded>(id);
			arm(task, base + std::chrono::milliseconds(2 * (id / per_time)));
			keep.push_back(std::move(task));
		}
		bool all = wait_for(tasks);
		std::lock_guard<std::mutex> lock(order_mutex);
		return all ? order : std::vector<int>{};
	};
	std::vector<int> from_wheel, from_queue;
	{
		RecordedSched sched("CHECK");
		from_wheel = fired([&](auto& task, Clock::time_point at) { sched.add(task, at); });
	}
	{
		HeapTimers<Recorded> timers;
		from_queue = fired([&](auto& task, Clock::time_point at) { timers.add(task, at); });
	}
	std::printf("same-results check: %d tasks armed in shuffled order, %d to a due time, %d due times 2 ms apart\n", tasks, per_time, tasks / per_time);
	bool ok = true;
	auto row = [&](const char* label, const std::vector<int>& got) {
		bool same = got == expected;
		std::printf("  %-14s %zu fired, %s\n", label, got.size(), same ? "in due order, ties in arming order" : "DIFFERENT ORDER OR TASKS MISSING");
		ok = ok && same;
	};
	row("wheel add()", from_wheel);
	row("queue", from_queue);
	return ok;
}

static void print(const char* label, std::vector<double>& s) {
	std::sort(s.begin(), s.end());
	std::printf("%-16s median %7.1f  min %7.1f  spread %.3f ns per task\n", label, s[2], s[0], s.back() / s[0]);
	std::fflush(stdout);
}

int main(int argc, char** argv) {
	bool compare = false;
	bool check_only = false;
	for (int i = 1; i < argc; ++i) {
		if (!std::strcmp(argv[i], "--compare")) {
			compare = true;
		} else if (!std::strcmp(argv[i], "--check")) {
			check_only = true;
		}
	}
	if (compare || check_only) {
		if (!check()) {
			std::printf("same-results check FAILED\n");
			return 1;
		}
		if (check_only) {
			return 0;
		}
	}
	constexpr long kPer = 200000;
	for (int threads : {1, 4}) {
		std::printf("--- %d producer thread(s), %ld tasks each\n", threads, kPer);
		for (Mode m : {Mode::make_only, Mode::add, Mode::post, Mode::post_ordered, Mode::queue}) {
			if (m == Mode::queue && !compare) {
				continue;
			}
			std::vector<double> s;
			for (int r = 0; r < 5; ++r) {
				s.push_back(run(m, threads, kPer));
			}
			print(name(m), s);
		}
	}
	if (compare) {
		std::printf("--- fire: %ld tasks due at one instant, first run to last run\n", kPer);
		for (bool wheel : {true, false}) {
			std::vector<double> s;
			for (int r = 0; r < 5; ++r) {
				s.push_back(fire(wheel, kPer));
			}
			if (*std::min_element(s.begin(), s.end()) < 0) {
				std::printf("%-16s did not complete in time\n", wheel ? "add()" : "queue");
				continue;
			}
			print(wheel ? "add()" : "queue", s);
		}
	}
	return 0;
}
