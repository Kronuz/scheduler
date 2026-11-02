// ThreadedScheduler: add() at one producer, and firing a backlog through the worker pool.
//
//   add:  one producer adds N tasks at increasing wakeup times; ns per add().
//   fire: N tasks due at one instant, dispatched to W workers; ns per task from the due
//         instant until the last one ran.
//
//   cmake -B build -DCMAKE_BUILD_TYPE=Release -DSCHEDULER_BENCH=ON && cmake --build build
//   build/scheduler_bench_pool MODE [N] [WORKERS] [ROUNDS]       (prints the median)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "scheduler.h"

using clk = std::chrono::steady_clock;

struct Task;
using Sched = ThreadedScheduler<Task>;

static std::atomic<unsigned long> g_ran{0};

struct Task : ScheduledTask<Sched, Task> {
	void operator()() { g_ran.fetch_add(1, std::memory_order_relaxed); }
};

static double add_round(unsigned n, unsigned workers) {
	Sched sched("BENCH", "B{:02}", workers);
	std::vector<std::shared_ptr<Task>> tasks(n);
	for (auto& t : tasks) {
		t = std::make_shared<Task>();
	}
	auto base = clk::now() + std::chrono::seconds(5);
	auto begin = clk::now();
	for (unsigned i = 0; i < n; ++i) {
		sched.add(tasks[i], base + std::chrono::microseconds(i));
	}
	auto end = clk::now();
	sched.finish(0);
	return double(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count()) / n;
}

static double fire_round(unsigned n, unsigned workers) {
	g_ran.store(0);
	Sched sched("BENCH", "B{:02}", workers);
	std::vector<std::shared_ptr<Task>> tasks(n);
	for (auto& t : tasks) {
		t = std::make_shared<Task>();
	}
	auto due = clk::now() + std::chrono::milliseconds(400);
	for (unsigned i = 0; i < n; ++i) {
		sched.add(tasks[i], due);
	}
	while (g_ran.load(std::memory_order_acquire) < n) {
		std::this_thread::sleep_for(std::chrono::microseconds(200));
	}
	auto end = clk::now();
	// The poll above adds up to 200 us; the run is n tasks long, so it is noise.
	sched.finish(0);
	return double(std::chrono::duration_cast<std::chrono::nanoseconds>(end - due).count()) / n;
}

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s add|fire [N] [WORKERS] [ROUNDS]\n", argv[0]);
		return 2;
	}
	bool fire = !std::strcmp(argv[1], "fire");
	unsigned n = argc > 2 ? unsigned(std::atoi(argv[2])) : 200000;
	unsigned workers = argc > 3 ? unsigned(std::atoi(argv[3])) : 4;
	int rounds = argc > 4 ? std::atoi(argv[4]) : 5;
	std::vector<double> v;
	for (int r = 0; r < rounds; ++r) {
		v.push_back(fire ? fire_round(n, workers) : add_round(n, workers));
	}
	std::sort(v.begin(), v.end());
	std::printf("%s n=%u workers=%u median %.1f ns/task (min %.1f max %.1f)\n", argv[1], n, workers, v[v.size() / 2], v.front(), v.back());
	return 0;
}
