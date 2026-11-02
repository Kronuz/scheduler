// Lost-task soak: producers add bursts of immediate and near-future tasks while the
// worker alternates between running and sleeping; every task must fire.
//
//   scheduler_soak [total_adds=2000000] [producers=1] [idle_ms=2] [mode=0]
//
// mode 0: random bursts with short pauses (above). mode 1: each round lets the
// worker go back to sleep,
// parks a far task (5 s) and an earlier one (30 ms), then adds 20000 tasks due now.
//
// Prints the number of tasks that never fired (after a grace period far shorter
// than the worker's 30 s idle cap, so a task parked behind a wrongly lengthened
// sleep is counted as lost) and exits non-zero if any did. Not part of ctest's
// default run beyond a short smoke (see CMakeLists.txt); run it with a large
// total_adds (and under TSan) as a soak.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "scheduler.h"

using namespace std::chrono_literals;

static std::atomic<long long> g_ran{0};
struct SoakTask;
using SoakSched = Scheduler<SoakTask, ThreadPolicyType::regular>;
using SoakTaskBase = ScheduledTask<SoakSched, SoakTask, ThreadPolicyType::regular>;
struct SoakTask : public SoakTaskBase {
	using SoakTaskBase::SoakTaskBase;
	void operator()() { g_ran.fetch_add(1, std::memory_order_relaxed); }
};

int main(int argc, char** argv) {
	long long total = argc > 1 ? std::atoll(argv[1]) : 2000000;
	int producers = argc > 2 ? std::atoi(argv[2]) : 1;
	int idle_ms = argc > 3 ? std::atoi(argv[3]) : 2;

	int mode = argc > 4 ? std::atoi(argv[4]) : 0;

	SoakSched sched("SOAK");
	if (mode == 1) {
		constexpr int kEach = 20000;
		long long lost = 0, adds = 0;
		for (long long round = 0; round < total / kEach; ++round) {
			std::this_thread::sleep_for(std::chrono::milliseconds(idle_ms));
			g_ran.store(0);
			auto far = std::make_shared<SoakTask>();
			sched.add(far, std::chrono::steady_clock::now() + 5s);
			sched.add(std::make_shared<SoakTask>(), std::chrono::steady_clock::now() + 30ms);
			std::this_thread::sleep_for(100ms);
			far->clear();
			// The 30 ms task has run by now unless the machine is starved; wait for it, so a late one is
			// not counted among the tasks below (the macos-x86_64 runner showed lost=-1).
			for (auto until = std::chrono::steady_clock::now() + 2s; g_ran.load() < 1 && std::chrono::steady_clock::now() < until;) {
				std::this_thread::sleep_for(1ms);
			}
			g_ran.store(0);
			for (int i = 0; i < kEach; ++i) {
				sched.add(std::make_shared<SoakTask>(), std::chrono::steady_clock::now());
			}
			adds += kEach;
			auto deadline = std::chrono::steady_clock::now() + 2s;
			while (g_ran.load() < kEach && std::chrono::steady_clock::now() < deadline) {
				std::this_thread::sleep_for(1ms);
			}
			lost += kEach - g_ran.load();
		}
		std::printf("adds=%lld lost=%lld\n", adds, lost);
		std::fflush(stdout);
		if (lost != 0) std::_Exit(1);
		sched.finish();
		return 0;
	}
	std::atomic<long long> added{0};
	std::vector<std::thread> threads;
	for (int p = 0; p < producers; ++p) {
		threads.emplace_back([&, p] {
			std::mt19937 g(1234 + p);
			long long mine = total / producers;
			long long done = 0;
			while (done < mine) {
				// A small burst, then a pause so the worker goes back to sleep.
				int burst = 1 + static_cast<int>(g() % 8);
				for (int i = 0; i < burst && done < mine; ++i, ++done) {
					auto due = std::chrono::steady_clock::now();
					switch (g() % 4) {
						case 0: due += std::chrono::microseconds(g() % 3000); break;
						case 1: due += std::chrono::milliseconds(1 + g() % 3); break;
						default: break;
					}
					if (sched.add(std::make_shared<SoakTask>(), due) == schedule_result::scheduled) {
						added.fetch_add(1, std::memory_order_relaxed);
					}
				}
				if (idle_ms > 0 && g() % 64 == 0) {
					std::this_thread::sleep_for(std::chrono::milliseconds(g() % (idle_ms + 1)));
				} else if (g() % 4 == 0) {
					std::this_thread::yield();
				}
			}
		});
	}
	for (auto& t : threads) t.join();

	auto deadline = std::chrono::steady_clock::now() + 3s;
	while (g_ran.load() < added.load() && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(5ms);
	}
	long long lost = added.load() - g_ran.load();
	std::printf("adds=%lld ran=%lld lost=%lld\n", added.load(), g_ran.load(), lost);
	std::fflush(stdout);
	if (lost != 0) std::_Exit(1);   // a lost task may still be pending; skip the join
	sched.finish();
	return 0;
}
