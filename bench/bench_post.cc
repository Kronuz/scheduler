// What a producer pays to arm a task: add() (the wheel insert on the caller's thread), post() (a hand-off
// the worker files into the wheel) and post_ordered() (a hand-off the worker keeps in an arrival queue),
// each for tasks due 1 ms after the call, with the worker running them meanwhile. The task is made in the
// loop in every case, so the "make only" row is the part they share.
//
// Build: cmake -B build -DSCHEDULER_BENCH=ON && cmake --build build --target scheduler_bench_post
// Time it under `quiet strict`.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "scheduler.h"

struct Trivial;
using BenchSched = Scheduler<Trivial, ThreadPolicyType::regular>;
using BenchBase = ScheduledTask<BenchSched, Trivial, ThreadPolicyType::regular>;

static std::atomic<long> ran;
struct Trivial : BenchBase {
	void operator()() { ran.fetch_add(1, std::memory_order_relaxed); }
};

static double now_s() {
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

enum class Mode { make_only, add, post, post_ordered };

static const char* name(Mode m) {
	switch (m) {
		case Mode::make_only: return "make only";
		case Mode::add: return "add()";
		case Mode::post: return "post()";
		default: return "post_ordered()";
	}
}

static double run(Mode mode, int threads, long per_thread) {
	BenchSched sched("BENCH");
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
				auto now = std::chrono::steady_clock::now();
				auto at = now + std::chrono::milliseconds(1);
				switch (mode) {
					case Mode::make_only: break;
					case Mode::add: sched.add(task, at); break;
					case Mode::post: sched.post(task, at, now); break;
					case Mode::post_ordered: sched.post_ordered(task, at, now); break;
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
	auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (ran.load() < want && std::chrono::steady_clock::now() < end) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return static_cast<double>(total_ns) / static_cast<double>(per_thread * threads);
}

int main() {
	constexpr long kPer = 200000;
	for (int threads : {1, 4}) {
		std::printf("--- %d producer thread(s), %ld tasks each\n", threads, kPer);
		for (Mode m : {Mode::make_only, Mode::add, Mode::post, Mode::post_ordered}) {
			std::vector<double> s;
			for (int r = 0; r < 5; ++r) {
				s.push_back(run(m, threads, kPer));
			}
			std::sort(s.begin(), s.end());
			std::printf("%-16s median %7.1f  min %7.1f  spread %.3f ns per task\n", name(m), s[2], s[0], s.back() / s[0]);
			std::fflush(stdout);
		}
	}
	return 0;
}
