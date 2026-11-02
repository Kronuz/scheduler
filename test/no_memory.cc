// no_memory: allocation failure inside the wheel or a debouncer touch is reported,
// not thrown, and leaves nothing pending; one inside the worker's arrival queue runs
// the task being filed if it is due, drops it otherwise, and leaves the worker
// running. It replaces global operator new to fail on demand; TSan's static runtime
// defines it too and the link clashes, so CMake skips this test in TSan builds (it
// runs under ASan+UBSan).
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
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
// Fails the next allocation of exactly this many bytes, on any thread; 0 = off.
static std::atomic<std::size_t> g_fail_size{0};
static std::atomic<bool> g_fail_nothrow{false};   // the next nothrow new returns null
// Counts the allocations this thread makes while set, and keeps the sizes of the first 32
// (an array: recording must not allocate).
static thread_local bool t_record = false;
static thread_local int t_recorded = 0;
static thread_local std::size_t t_sizes[32];

static void* allocate(std::size_t n) {
	if (t_fail) {
		throw std::bad_alloc();
	}
	if (t_record) {
		if (t_recorded < 32) {
			t_sizes[t_recorded] = n;
		}
		++t_recorded;
	}
	if (auto size = g_fail_size.load(std::memory_order_relaxed); size == n &&
	    g_fail_size.compare_exchange_strong(size, 0)) {
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
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { if (g_fail_nothrow.exchange(false)) { return nullptr; } try { return allocate(n); } catch (...) { return nullptr; } }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { try { return allocate(n); } catch (...) { return nullptr; } }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { try { return allocate(n, a); } catch (...) { return nullptr; } }
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { try { return allocate(n, a); } catch (...) { return nullptr; } }
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

// How the worker's arrival queue (a std::deque of shared_ptr) gets a new block: the size of the
// largest request, and the number of push_backs after construction that makes it. libc++
// allocates at the first push_back, libstdc++ at construction and at every 32nd push_back.
struct ArrivalsBlock {
	std::size_t size = 0;
	int pushes = 0;
};

static ArrivalsBlock arrivals_block() {
	t_recorded = 0;
	t_record = true;
	std::deque<std::shared_ptr<CountTask>> probe;   // the construction itself may allocate
	const int mark = t_recorded;
	ArrivalsBlock block;
	while (t_recorded == mark && block.pushes < 4096) {
		probe.push_back(nullptr);
		++block.pushes;
	}
	t_record = false;
	for (int i = mark; i < t_recorded && i < 32; ++i) {
		if (t_sizes[i] > block.size) {
			block.size = t_sizes[i];
		}
	}
	return block;
}

static bool wait_until(auto&& done, std::chrono::milliseconds limit) {
	for (auto until = std::chrono::steady_clock::now() + limit; std::chrono::steady_clock::now() < until;) {
		if (done()) {
			return true;
		}
		std::this_thread::sleep_for(2ms);
	}
	return done();
}

// An allocation failure in the worker's arrival queue (LPH-R6), when the task being filed is due and
// when it is not. A refused wheel insert is handled the same way: a due task runs, any other is
// dropped, and the worker goes on. Without the handler the exception left the worker's loop, the
// thread ended, and every later post() returned `scheduled` and never ran.
static void test_arrivals_no_memory(bool due) {
	const ArrivalsBlock block = arrivals_block();
	CHECK(block.size != 0 && block.pushes >= 1);

	Sched sched("ARRIVALS");
	std::this_thread::sleep_for(30ms);
	g_ran = 0;

	// Fill the queue up to the push that needs a new block; each of these tasks is due and runs.
	for (int i = 1; i < block.pushes; ++i) {
		auto task = std::make_shared<CountTask>();
		auto now = std::chrono::steady_clock::now();
		CHECK(sched.post_ordered(task, now, now) == schedule_result::scheduled);
		CHECK(wait_until([&] { return g_ran.load() == i; }, 2s));
	}
	const int before = g_ran.load();

	auto victim = std::make_shared<CountTask>();
	auto after = std::make_shared<CountTask>();
	auto now = std::chrono::steady_clock::now();
	const auto late = now + 500ms;
	g_fail_size = block.size;   // the worker's push_back of `victim` asks for a new block and is refused
	CHECK(sched.post_ordered(victim, due ? now : late, now) == schedule_result::scheduled);
	CHECK(wait_until([&] { return g_fail_size.load() == 0; }, 2s));   // the failure landed
	if (due) {
		CHECK(wait_until([&] { return g_ran.load() == before + 1; }, 2s));   // it ran, at once
	} else {
		std::this_thread::sleep_for(800ms);   // well past its time
		CHECK(g_ran.load() == before);          // dropped: it never runs
		CHECK(victim.use_count() == 1);         // and the scheduler holds no reference to it
	}

	// The worker is still working: a task posted after the failure is filed and runs.
	now = std::chrono::steady_clock::now();
	CHECK(sched.post_ordered(after, due ? now : now + 100ms, now) == schedule_result::scheduled);
	CHECK(wait_until([&] { return g_ran.load() == before + (due ? 2 : 1); }, 2s));
	sched.finish();
	CHECK(g_ran.load() == before + (due ? 2 : 1));
}

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

	// A re-arm refused for lack of memory in the fire path (on a worker, with no
	// caller to tell) runs the call now, once, with the latest arguments, instead
	// of at its target, and releases the key.
	{
		std::mutex mtx;
		std::vector<int> calls;
		auto deb = make_debouncer<int>(
			"NMF", "NF{:02}", 1,
			[&](int, int value) {
				std::lock_guard<std::mutex> lk(mtx);
				calls.push_back(value);
			},
			0ms, 50ms, 400ms, 1s, 1s);
		auto start = std::chrono::steady_clock::now();
		CHECK(deb.debounce(1, 1, 10) == schedule_result::scheduled);   // fires at ~50 ms
		CHECK(deb.debounce(1, 1, 11) == schedule_result::scheduled);   // target ~400 ms
		g_fail_size = sizeof(std::shared_ptr<int>);                       // the re-arm's value box
		for (auto until = start + 5s; std::chrono::steady_clock::now() < until;) {
			{
				std::lock_guard<std::mutex> lk(mtx);
				if (!calls.empty()) { break; }
			}
			std::this_thread::sleep_for(5ms);
		}
		auto elapsed = std::chrono::steady_clock::now() - start;
		std::this_thread::sleep_for(100ms);
		CHECK(g_fail_size.load() == 0);   // the re-arm hit the injected failure
		CHECK(deb.pending_keys() == 0);
		deb.finish();
		std::lock_guard<std::mutex> lk(mtx);
		CHECK(calls.size() == 1 && calls[0] == 11);
		CHECK(elapsed < 350ms);           // ran at the early fire, not at the target
	}

	// An allocation failure in the worker's arrival queue: a due task runs, any other is dropped,
	// and the worker keeps working.
	test_arrivals_no_memory(true);
	test_arrivals_no_memory(false);

	// A due task whose dispatch cannot be allocated is counted as refused and never runs; the next
	// one runs.
	{
		struct Sink;
		using SinkSched = ThreadedScheduler<Sink, ThreadPolicyType::regular>;
		struct Sink : ScheduledTask<SinkSched, Sink, ThreadPolicyType::regular> {
			void operator()() { g_ran.fetch_add(1); }
		};
		SinkSched sched("NMT", "NT{:02}", 2);
		auto before = g_ran.load();
		CHECK(sched.add(std::make_shared<Sink>(), std::chrono::steady_clock::now() + 100ms) == schedule_result::scheduled);
		g_fail_nothrow = true;
		CHECK(wait_until([&] { return !g_fail_nothrow.load(); }, 2s));   // the dispatch hit the failure
		std::this_thread::sleep_for(100ms);
		CHECK(sched.refused_size() == 1);
		CHECK(g_ran.load() == before);
		CHECK(sched.add(std::make_shared<Sink>(), std::chrono::steady_clock::now() + 20ms) == schedule_result::scheduled);
		CHECK(wait_until([&] { return g_ran.load() == before + 1; }, 2s));
		CHECK(sched.refused_size() == 1);
		sched.finish();
	}

	std::printf("no_memory OK: add and debounce report it and leave nothing pending; the worker survives it in its arrival queue\n");
	return 0;
}
