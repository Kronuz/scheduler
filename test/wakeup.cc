#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace probe {
enum class Mode { Enqueue, Shutdown, Reentrant, NoNotify, Reschedule };
std::mutex mutex;
std::condition_variable changed;
std::atomic<bool> first{true};
Mode mode = Mode::Enqueue;
bool paused = false, completed = false, released = false;
unsigned fired = 0, no_notify = 0;
void (*reenter)() = nullptr;
void trace(std::string_view text) {
	if (text.find("\"ADDED\"") != std::string_view::npos) {
		std::lock_guard lock(mutex);
		++no_notify;
	}
}
void hook(std::string_view name) {
	const auto site = mode == Mode::Reentrant || mode == Mode::NoNotify || mode == Mode::Shutdown
	    ? "BaseScheduler::LOOP" : "BaseScheduler::BEFORE_PROPOSE";
	if (name == site && first.exchange(false)) {
		std::unique_lock lock(mutex);
		paused = true;
		changed.notify_all();
		changed.wait(lock, [] { return released; });
		const bool call = mode == Mode::Reentrant;
		lock.unlock();
		if (call) reenter();
	}
}
}
#define L_DEBUG_HOOK(key, ...) probe::hook(key)
#define L_SCHEDULER(...) probe::trace(#__VA_ARGS__)
#include "scheduler.h"
using namespace std::chrono_literals;
struct Task;
Scheduler<Task> *active = nullptr;
struct Task : ScheduledTask<Scheduler<Task>, Task> {
	void operator()() {
		bool again = false;
		{
			std::lock_guard lock(probe::mutex);
			++probe::fired;
			again = probe::mode == probe::Mode::Reschedule && probe::fired == 1;
			probe::changed.notify_all();
		}
		if (again) active->add(std::make_shared<Task>(), std::chrono::steady_clock::now());
	}
};
void reenter() { active->add(std::make_shared<Task>(), std::chrono::steady_clock::now()); }

bool run(probe::Mode mode) {
	probe::mode = mode;
	probe::first = true;
	probe::paused = probe::completed = probe::released = false;
	probe::fired = probe::no_notify = 0;
	probe::reenter = reenter;
	Scheduler<Task> scheduler("WAKE");
	std::unique_lock lock(probe::mutex);
	active = &scheduler;
	const bool paused = probe::changed.wait_for(lock, 5s, [] { return probe::paused; });
	lock.unlock();
	std::thread producer([&] {
		if (mode == probe::Mode::Shutdown) {
			scheduler.finish(0);
		} else if (mode == probe::Mode::NoNotify) {
			scheduler.add(std::make_shared<Task>(), std::chrono::steady_clock::now() + 45s);
			scheduler.finish(0);
		} else if (mode == probe::Mode::Enqueue || mode == probe::Mode::Reschedule) {
			scheduler.add(std::make_shared<Task>(), std::chrono::steady_clock::now());
		}
		std::lock_guard done(probe::mutex);
		probe::completed = true;
		probe::changed.notify_all();
	});
	lock.lock();
	const bool completed = probe::changed.wait_for(lock, 5s, [] { return probe::completed; });
	probe::released = true;
	probe::changed.notify_all();
	const bool stopping = mode == probe::Mode::Shutdown || mode == probe::Mode::NoNotify;
	const unsigned expected = mode == probe::Mode::Reschedule ? 2u : 1u;
	const bool fired = stopping || probe::changed.wait_for(lock, 5s, [&] { return probe::fired == expected; });
	lock.unlock();
	producer.join();
	const bool joined = stopping ? scheduler.join(5s) : scheduler.finish();
	if (!joined) scheduler.finish();
	lock.lock();
	const bool counts = stopping ? probe::fired == 0 : probe::fired == expected;
	const bool branch = mode != probe::Mode::NoNotify || probe::no_notify == 1;
	const bool ok = paused && completed && fired && joined && counts && branch;
	if (!ok)
		std::fprintf(stderr, "wakeup case %u failed: paused=%d completed=%d fired=%d joined=%d counts=%d branch=%d\n",
		             unsigned(mode), paused, completed, fired, joined, counts, branch);
	return ok;
}
int main(int argc, char **argv) {
	bool ok = true;
	for (const auto mode : {probe::Mode::Enqueue, probe::Mode::Shutdown,
	                        probe::Mode::Reentrant, probe::Mode::NoNotify, probe::Mode::Reschedule}) {
		if (argc > 1 && std::string_view(argv[1]) != std::to_string(unsigned(mode))) continue;
		ok = run(mode) && ok;
	}
	if (ok) std::puts("enqueue, shutdown, tracing, no-notify and callback rescheduling passed");
	return ok ? 0 : 1;
}
