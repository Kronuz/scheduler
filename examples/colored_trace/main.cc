// Demonstrates that scheduler's tracing/coloring is fully recoverable by a
// consumer. This build injects a trace header (trace.h) that turns scheduler.h's
// no-op hooks into colored, std::format-rendered trace output, and forces the
// color identifiers (BROWN, CLEAR_COLOR, DODGER_BLUE, ...) to actually compile.
// With no SCHEDULER_TRACE_HEADER (the default), the exact same scheduler.h
// produces no trace and no color, and those identifiers are never referenced.
//
// Build & run (from this directory; threadpool.hh / stash.h come from a built
// tree of the deps — see README.md for the full include flags):
//   see README.md
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

#include "scheduler.h"

using namespace std::chrono_literals;

struct DemoTask;
using DemoSched = Scheduler<DemoTask, ThreadPolicyType::regular>;
using DemoBase = ScheduledTask<DemoSched, DemoTask, ThreadPolicyType::regular>;

struct DemoTask : public DemoBase {
	int id;
	explicit DemoTask(int id) : id(id) {}
	void operator()() {
		std::printf("  >> task %d fired\n", id);
	}
};

int main() {
	std::puts("=== scheduler with a colored trace header injected ===");

	DemoSched sched("DEMO");
	auto now = std::chrono::steady_clock::now();

	sched.add(std::make_shared<DemoTask>(1), now + 80ms);
	sched.add(std::make_shared<DemoTask>(2), now + 40ms);

	std::this_thread::sleep_for(250ms);
	sched.finish();

	std::puts("=== done (the colored lines above came through scheduler's trace hooks) ===");
	return 0;
}
