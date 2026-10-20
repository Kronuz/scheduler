// no_memory: allocation failure inside the wheel or a debouncer touch is reported,
// not thrown, and leaves nothing pending. It replaces global operator new to fail
// on demand, which the sanitizer runtimes also define, so CMake skips it there.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "debouncer.h"
#include "scheduler.h"

using namespace std::chrono_literals;

// Fails every allocation on this thread while set.
static thread_local bool t_fail = false;

static void* allocate(std::size_t n) {
	if (t_fail) {
		throw std::bad_alloc();
	}
	if (auto p = std::malloc(n ? n : 1)) {
		return p;
	}
	throw std::bad_alloc();
}

static void* allocate(std::size_t n, std::align_val_t a) {
	if (t_fail) {
		throw std::bad_alloc();
	}
	void* p = nullptr;
	auto al = static_cast<std::size_t>(a) < sizeof(void*) ? sizeof(void*) : static_cast<std::size_t>(a);
	if (posix_memalign(&p, al, n ? n : 1) == 0) {
		return p;
	}
	throw std::bad_alloc();
}

void* operator new(std::size_t n) { return allocate(n); }
void* operator new[](std::size_t n) { return allocate(n); }
void* operator new(std::size_t n, std::align_val_t a) { return allocate(n, a); }
void* operator new[](std::size_t n, std::align_val_t a) { return allocate(n, a); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }

#define CHECK(cond) do { \
	if (!(cond)) { std::fprintf(stderr, "CHECK FAILED at line %d: %s\n", __LINE__, #cond); std::abort(); } \
} while (0)

struct CountTask;
using Sched = Scheduler<CountTask, ThreadPolicyType::regular>;
static std::atomic<int> g_ran{0};
struct CountTask : public ScheduledTask<Sched, CountTask, ThreadPolicyType::regular> {
	void operator()() { g_ran.fetch_add(1); }
};

int main() {
	// The wheel's first insert allocates its top chunk: no_memory, nothing fires.
	{
		Sched sched("NOMEM");
		auto refused = std::make_shared<CountTask>();
		auto when = std::chrono::steady_clock::now() + 50ms;
		t_fail = true;
		auto result = sched.add(refused, when);
		t_fail = false;
		CHECK(result == schedule_result::no_memory);
		CHECK(sched.add(std::make_shared<CountTask>(), std::chrono::steady_clock::now() + 50ms) == schedule_result::scheduled);
		for (int i = 0; i < 100 && g_ran.load() < 1; ++i) {
			std::this_thread::sleep_for(20ms);
		}
		sched.finish();
		CHECK(g_ran.load() == 1);
	}

	// A touch that cannot allocate: no_memory and no key left pending. A touch on a
	// pending key whose arguments cannot be copied: no_memory, and the pending call
	// keeps its earlier arguments and fires once.
	{
		std::mutex mtx;
		std::vector<std::string> calls;
		auto deb = make_debouncer<int>(
			"NMD", "NM{:02}", 2,
			[&](int, std::string value) {
				std::lock_guard<std::mutex> lk(mtx);
				calls.push_back(std::move(value));
			},
			0ms, 50ms, 50ms, 150ms, 150ms);
		std::string first(64, 'a'), second(64, 'b');
		t_fail = true;
		auto result = deb.debounce(1, 1, first);
		t_fail = false;
		CHECK(result == schedule_result::no_memory);
		CHECK(deb.pending_keys() == 0);

		CHECK(deb.debounce(2, 2, first) == schedule_result::scheduled);
		t_fail = true;
		result = deb.debounce(2, 2, second);
		t_fail = false;
		CHECK(result == schedule_result::no_memory);
		CHECK(deb.pending_keys() == 1);
		for (auto until = std::chrono::steady_clock::now() + 5s; std::chrono::steady_clock::now() < until;) {
			{
				std::lock_guard<std::mutex> lk(mtx);
				if (!calls.empty()) { break; }
			}
			std::this_thread::sleep_for(10ms);
		}
		std::this_thread::sleep_for(100ms);
		deb.finish();
		std::lock_guard<std::mutex> lk(mtx);
		CHECK(calls.size() == 1 && calls[0] == first);
	}

	std::printf("no_memory OK: add and debounce report it and leave nothing pending\n");
	return 0;
}
