// Cancel churn on a debouncer: Xapiand's committer pattern.
//
// Producer threads touch random keys at a fixed total rate; each touch is a
// debounce(key, args). Today a touch on a key with a pending task cancels it and
// adds a new one, so the wheel keeps every cancelled task (its heap box, the task,
// the key and the arguments) until the walk drains its slot. The bench samples, once
// a second, the bytes live on the heap, the process RSS, the debouncer tasks alive
// (one per arguments object), and the calls fired, then stops touching and keeps
// sampling while the wheel drains. At the end it prints the process CPU time.
//
//   bench_churn [SECONDS] [RATE] [KEYS] [THREADS] [DRAIN_SECONDS]
//   bench_churn 30 100000 1000 2 12
//
// Timeouts are the committer's defaults (opts.cc): debounce 1 s, busy 3 s, forced
// within 8 to 10 s, no throttle.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <sys/resource.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <malloc/malloc.h>
static size_t usable(void* p) { return malloc_size(p); }
static size_t rss_bytes() {
	mach_task_basic_info info;
	mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
	if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &n) != KERN_SUCCESS) return 0;
	return info.resident_size;
}
#else
#include <malloc.h>
#include <unistd.h>
static size_t usable(void* p) { return malloc_usable_size(p); }
static size_t rss_bytes() {
	long pages = 0, resident = 0;
	if (FILE* f = std::fopen("/proc/self/statm", "r")) {
		if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
		std::fclose(f);
	}
	return (size_t)resident * (size_t)sysconf(_SC_PAGESIZE);
}
#endif

// Heap accounting by usable size, so aligned and sized deletes need no header.
static std::atomic<long long> g_live{0};
static void* track(void* p) {
	if (!p) throw std::bad_alloc();
	g_live.fetch_add((long long)usable(p), std::memory_order_relaxed);
	return p;
}
static void untrack(void* p) noexcept {
	if (!p) return;
	g_live.fetch_sub((long long)usable(p), std::memory_order_relaxed);
	std::free(p);
}
void* operator new(std::size_t n) { return track(std::malloc(n ? n : 1)); }
void* operator new[](std::size_t n) { return track(std::malloc(n ? n : 1)); }
void* operator new(std::size_t n, std::align_val_t a) {
	void* p = nullptr;
	if (posix_memalign(&p, std::max<std::size_t>((std::size_t)a, sizeof(void*)), n ? n : 1)) p = nullptr;
	return track(p);
}
void* operator new[](std::size_t n, std::align_val_t a) { return operator new(n, a); }
void operator delete(void* p) noexcept { untrack(p); }
void operator delete[](void* p) noexcept { untrack(p); }
void operator delete(void* p, std::size_t) noexcept { untrack(p); }
void operator delete[](void* p, std::size_t) noexcept { untrack(p); }
void operator delete(void* p, std::align_val_t) noexcept { untrack(p); }
void operator delete[](void* p, std::align_val_t) noexcept { untrack(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { untrack(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { untrack(p); }

#include "debouncer.h"

using namespace std::chrono_literals;

// The committer's argument is a weak_ptr<Shard>; this one also counts itself.
static std::atomic<long long> g_args{0};
struct Args {
	std::weak_ptr<int> shard;
	Args() { g_args.fetch_add(1, std::memory_order_relaxed); }
	Args(const Args& o) : shard(o.shard) { g_args.fetch_add(1, std::memory_order_relaxed); }
	Args(Args&& o) noexcept : shard(std::move(o.shard)) { g_args.fetch_add(1, std::memory_order_relaxed); }
	Args& operator=(const Args&) = default;
	Args& operator=(Args&&) noexcept = default;
	~Args() { g_args.fetch_sub(1, std::memory_order_relaxed); }
};

static std::atomic<long long> g_fired{0};

int main(int argc, char** argv) {
	double seconds = argc > 1 ? atof(argv[1]) : 30.0;
	long rate = argc > 2 ? atol(argv[2]) : 100000;
	int nkeys = argc > 3 ? atoi(argv[3]) : 1000;
	int nthreads = argc > 4 ? atoi(argv[4]) : 2;
	double drain = argc > 5 ? atof(argv[5]) : 12.0;

	std::vector<std::string> keys;
	keys.reserve(nkeys);
	for (int i = 0; i < nkeys; ++i) {
		char buf[64];
		std::snprintf(buf, sizeof buf, "/var/db/xapiand/index-%05d/.shard.%02d", i, i % 7);
		keys.emplace_back(buf);
	}
	auto shard = std::make_shared<int>(0);

	auto base_live = g_live.load();
	auto debouncer = make_unique_debouncer<std::string>("CH--", "CH{:02}", 2,
		[](Args) { g_fired.fetch_add(1, std::memory_order_relaxed); },
		0ms, 1000ms, 3000ms, 8000ms, 10000ms);

	std::atomic<bool> stop{false};
	std::atomic<long long> touches{0};
	std::vector<std::thread> producers;
	for (int t = 0; t < nthreads; ++t) {
		producers.emplace_back([&, t] {
			std::mt19937_64 rng(0x9E3779B97F4A7C15ULL ^ (uint64_t)t);
			std::uniform_int_distribution<int> pick(0, nkeys - 1);
			double per_ms = (double)rate / nthreads / 1000.0;
			double owed = 0;
			auto next = std::chrono::steady_clock::now();
			while (!stop.load(std::memory_order_relaxed)) {
				owed += per_ms;
				for (; owed >= 1; owed -= 1) {
					Args a;
					a.shard = shard;
					debouncer->debounce(keys[pick(rng)], std::move(a));
					touches.fetch_add(1, std::memory_order_relaxed);
				}
				next += 1ms;
				std::this_thread::sleep_until(next);
			}
		});
	}

	std::printf("bench_churn: %.0fs at %ld touches/s over %d keys (%d threads), then %.0fs drain; committer timeouts 1s/3s/8-10s\n",
	            seconds, rate, nkeys, nthreads, drain);
	std::printf("     t |   touches/s |  fired/s |  args alive |  heap MB |   RSS MB\n");
	auto t0 = std::chrono::steady_clock::now();
	long long last_touches = 0, last_fired = 0;
	long long peak_args = 0, peak_live = 0;
	size_t peak_rss = 0;
	int total = (int)(seconds + drain);
	for (int s = 1; s <= total; ++s) {
		std::this_thread::sleep_until(t0 + std::chrono::seconds(s));
		if (s == (int)seconds) {
			stop.store(true);
			for (auto& p : producers) p.join();
		}
		auto tc = touches.load(), fc = g_fired.load(), ac = g_args.load();
		auto live = g_live.load() - base_live;
		auto rss = rss_bytes();
		peak_args = std::max(peak_args, ac);
		peak_live = std::max(peak_live, live);
		peak_rss = std::max(peak_rss, rss);
		std::printf("%6d | %11lld | %8lld | %11lld | %8.1f | %8.1f\n", s, tc - last_touches, fc - last_fired, ac,
		            live / 1048576.0, rss / 1048576.0);
		std::fflush(stdout);
		last_touches = tc;
		last_fired = fc;
	}
	rusage ru{};
	getrusage(RUSAGE_SELF, &ru);
	double cpu = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
	std::printf("peak: args alive %lld, heap %.1f MB, RSS %.1f MB; touches %lld, fired %lld; CPU %.2f s\n",
	            peak_args, peak_live / 1048576.0, peak_rss / 1048576.0, touches.load(), g_fired.load(), cpu);
	debouncer.reset();
	return 0;
}
