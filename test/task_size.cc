// The size of ScheduledTask and the two stages of its hand-off fields.
//
//   1. A task is 72 bytes on a 64-bit target: the 48 it had before post() plus the 24 bytes of the
//      hand-off fields. Every task a caller makes pays for its bytes, in the allocation and in what its
//      constructor writes, so a field added beside them fails this.
//   2. The hand-off fields (the posted stage) and the filing fields (the filed stage) share storage.
//      A task that was never posted, and one whose posted stage has ended, read zero as filed: that is
//      what BaseScheduler::before() takes for "never filed", and a stale link would order it wrongly.
//   3. The posted stage holds the task until the worker takes the hold, and takes it exactly once.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "scheduler.h"

#define CHECK(cond) do { \
	if (!(cond)) { \
		std::fprintf(stderr, "CHECK FAILED at line %d: %s\n", __LINE__, #cond); \
		std::abort(); \
	} \
} while (0)

struct Probe;
using ProbeSched = Scheduler<Probe, ThreadPolicyType::regular>;
using ProbeBase = ScheduledTask<ProbeSched, Probe, ThreadPolicyType::regular>;

// Opens the protected stages of the hand-off to the test.
struct Probe : ProbeBase {
	using ProbeBase::begin_posted;
	using ProbeBase::end_posted;
	using ProbeBase::hand;
	void operator()() {}
};

static ProbeBase* address(std::uintptr_t value) {
	return reinterpret_cast<ProbeBase*>(value);
}

static void test_size() {
	std::printf("sizeof(ScheduledTask) %zu\n", sizeof(ProbeBase));
	if (sizeof(void*) == 8) {
		CHECK(sizeof(ProbeBase) == 72);
	}
}

static void test_stages() {
	auto task = std::make_shared<Probe>();
	CHECK(task->hand.filed.seq == 0);
	CHECK(task->hand.filed.tick == 0);

	// The posted stage holds the task: a reference beside the caller's, and a link that starts empty.
	task->begin_posted(task);
	CHECK(task.use_count() == 2);
	CHECK(task->hand.posted.next == nullptr);

	// The poster links it behind another; the worker ends the stage and gets the hold, once.
	task->hand.posted.next = address(0x1000);
	auto hold = task->end_posted();
	CHECK(hold.get() == static_cast<ProbeBase*>(task.get()));
	CHECK(task.use_count() == 2);   // the caller's and `hold`: the stage keeps none
	hold.reset();
	CHECK(task.use_count() == 1);

	// The filed stage starts at zero: the link the posted stage held is not a filing number.
	CHECK(task->hand.filed.seq == 0);
	CHECK(task->hand.filed.tick == 0);

	// The worker writes it; a refused post (the stack closed) ends the stage the same way, zeroed.
	task->hand.filed.seq = 7;
	task->hand.filed.tick = 123456789;
	CHECK(task->hand.filed.seq == 7);
	CHECK(task->hand.filed.tick == 123456789);
	task->begin_posted(task);
	task->hand.posted.next = address(1);
	task->end_posted().reset();
	CHECK(task.use_count() == 1);
	CHECK(task->hand.filed.seq == 0);
	CHECK(task->hand.filed.tick == 0);
	std::printf("hand-off stages OK\n");
}

int main() {
	test_size();
	test_stages();
	std::printf("all task size tests passed\n");
	return 0;
}
