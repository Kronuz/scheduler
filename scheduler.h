/*
 * Copyright © 2026 Germán Méndez Bravo (Kronuz)
 * Copyright (c) 2015-2019 Dubalu LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#pragma once

#include <atomic>                 // for std::atomic
#include <cassert>                // for assert
#include <chrono>                 // for std::chrono
#include <cstdint>                // for std::uint8_t
#include <deque>                  // for std::deque
#include <memory>
#include <mutex>                  // for std::condition_variable, std::mutex
#include <string>                 // for std::string
#include <utility>                // for std::move

#include "stash.h"                // for StashValues, StashSlots, StashContext
#include "thread.hh"              // for Thread, ThreadPolicyType
#include "threadpool.hh"          // for ThreadPool

// Tracing is optional and fully injectable. Define SCHEDULER_TRACE_HEADER (a
// header path) before including this file to plug in your own logging macros
// (L_SCHEDULER / L_DEBUG_HOOK / L_EXC / L_CALL) and the color identifiers they
// reference; otherwise the bundled no-op stubs in scheduler_trace.h are used.
// See scheduler_trace.h and examples/colored_trace/.
#ifdef SCHEDULER_TRACE_HEADER
#  include SCHEDULER_TRACE_HEADER
#else
#  include "scheduler_trace.h"
#endif


using namespace std::chrono_literals;


// Test seam in post(): between the poster's `stopping` check and its push (a preempted
// poster stands there). A no-op by default; a test defines it to hold a poster at that point.
#ifndef SCHEDULER_PUSH_HOOK
#define SCHEDULER_PUSH_HOOK() ((void)0)
#endif

// Test seam in the worker's loop: a no-op by default. The tests define it to park the worker
// at a named point, which makes the add()/sleep races deterministic. At "after_peep", `stale`
// is true when the worker found nothing pending and its local copy of the published wakeup
// time still equals the published one, so its next compare-exchange will succeed. At "sleep"
// (the worker is about to take the mutex and announce `sleeping`), it is true when the
// worker's own look found a task already due, which it never does: it walks at once instead.
#ifndef SCHEDULER_TEST_HOOK
#define SCHEDULER_TEST_HOOK(site, stale) ((void)0)
#endif


// #define L_SCHEDULER L_PRINT


#ifndef L_SCHEDULER
#define L_SCHEDULER_DEFINED
#define L_SCHEDULER L_NOTHING
#endif


// What add() (and Debouncer::debounce()) did with a task: only `scheduled` fires.
enum class schedule_result : std::uint8_t {
	scheduled,      // on the wheel; fires at its wakeup time unless cleared
	past_horizon,   // more than the wheel's 24 h span ahead
	stopping,       // finish() has begun
	no_memory,      // the wheel could not allocate before claiming a slot
};


template <typename SchedulerImpl, typename ScheduledTaskImpl, ThreadPolicyType thread_policy>
class BaseScheduler;

template <typename ScheduledTaskImpl, ThreadPolicyType thread_policy>
class Scheduler;

template <typename ScheduledTaskImpl, ThreadPolicyType thread_policy>
class ThreadedScheduler;


template <typename SchedulerImpl, typename ScheduledTaskImpl, ThreadPolicyType thread_policy = ThreadPolicyType::regular>
class ScheduledTask : public std::enable_shared_from_this<ScheduledTask<SchedulerImpl, ScheduledTaskImpl, thread_policy>> {
	friend BaseScheduler<SchedulerImpl, ScheduledTaskImpl, thread_policy>;
	friend Scheduler<ScheduledTaskImpl, thread_policy>;
	friend ThreadedScheduler<ScheduledTaskImpl, thread_policy>;

protected:
	std::chrono::steady_clock::time_point wakeup_time;
	std::atomic<std::chrono::steady_clock::time_point> atom_created_at;
	std::atomic<std::chrono::steady_clock::time_point> atom_cleared_at;
	// Set true only by a *user* clear() (internal == false). The scheduler's own
	// one-shot claim (clear(true)) leaves this false. The worker re-reads it at
	// execution time so a task cancelled AFTER it was dispatched to the pool, but
	// BEFORE it runs, does not fire — honoring "if it hasn't run yet, it never
	// will." See ScheduledTask::operator()/cancelled() and ThreadedScheduler.
	std::atomic_bool atom_cancelled;

	// The hand-off of BaseScheduler::post(): while the task waits for the worker to
	// file it into the wheel, the link to the task posted before it and the owning
	// reference the hand-off holds. Written by the poster before it publishes the task
	// and by the worker after it takes the task; no other code touches them.
	ScheduledTask* post_next = nullptr;
	std::shared_ptr<ScheduledTask> post_hold;
	bool post_ordered = false;           // post_ordered(): kept in the arrival queue, not the wheel
	std::uint64_t ingest_seq = 0;        // the worker's count of tasks filed, to merge the queue with the wheel
	unsigned long long ingest_tick = 0;  // the tick the task was filed in (its due tick, or the wheel's floor if overdue)

public:
	ScheduledTask(std::chrono::steady_clock::time_point created_at = std::chrono::steady_clock::now()) :
		wakeup_time(std::chrono::steady_clock::time_point{}),
		atom_created_at(created_at),
		atom_cleared_at(std::chrono::steady_clock::time_point{}),
		atom_cancelled(false) {}

	operator bool() const noexcept {
		return atom_cleared_at.load() == std::chrono::steady_clock::time_point{};
	}

	// True once a user clear() has run, regardless of who won the one-shot CAS.
	bool cancelled() const noexcept {
		return atom_cancelled.load(std::memory_order_acquire);
	}

	void operator()() {
		// Re-check user cancellation right before invoking the callback. On the
		// ThreadedScheduler path the wheel thread claims the one-shot (clear(true))
		// and THEN hands the task to a worker; a user clear() landing in that gap
		// loses the time-CAS but still flips atom_cancelled, so we must not run.
		if (cancelled()) {
			return;
		}
		static_cast<ScheduledTaskImpl*>(this)->operator()();
	}

	bool clear(bool internal = false) {
		// A user cancel always records intent, even if the scheduler already won
		// the one-shot CAS (dispatched-but-not-yet-run case). The internal one-shot
		// claim must NOT mark the task cancelled, or every dispatched task would be
		// suppressed by the worker's re-check.
		if (!internal) {
			atom_cancelled.store(true, std::memory_order_release);
		}
		std::chrono::steady_clock::time_point c;
		return atom_cleared_at.compare_exchange_strong(c, std::chrono::steady_clock::now());
	}
};


#define MS 1000000ULL


template <typename SchedulerImpl, typename ScheduledTaskImpl, ThreadPolicyType thread_policy>
class SchedulerQueue {
	using TaskType = std::shared_ptr<ScheduledTask<SchedulerImpl, ScheduledTaskImpl, thread_policy>>;

public:
	static unsigned long long now() {
		return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	// The wheel's resolution, and the start of the finest slot a time falls in: the wheel
	// fires a task once its slot has begun, so two tasks in one slot fire together, in the
	// order they were filed.
	static constexpr unsigned long long tick = 1ULL * MS;
	static unsigned long long tick_start(std::chrono::steady_clock::time_point time_point) {
		return static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(time_point.time_since_epoch()).count()) / tick * tick;
	}

	// The tick the wheel files a task due at `time_point` in, as add() would right now: a time
	// below the floor the last walk published (an overdue task) is filed at the floor. Only the
	// walking thread may call it (it is the one that moves the floor).
	unsigned long long filing_tick(std::chrono::steady_clock::time_point time_point) const noexcept {
		auto key = static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(time_point.time_since_epoch()).count());
		auto floor = ctx.atom_insert_floor.load(std::memory_order_relaxed);
		return (key < floor ? floor : key) / tick * tick;
	}

private:
	/*                               <  _Tp        _Size       _Div         _Mod  >*/
	using _tasks =        StashValues<TaskType,    10ULL>;
	using _50_1ms =       StashSlots<_tasks,       10ULL,       1ULL * MS,    50ULL>;
	using _10_50ms =      StashSlots<_50_1ms,      10ULL,      50ULL * MS,    10ULL>;
	using _36_500ms =     StashSlots<_10_50ms,     12ULL,     500ULL * MS,    36ULL>;
	using _4800_18s =     StashSlots<_36_500ms,  4800ULL,   18000ULL * MS,  4800ULL>;

	StashContext ctx;
	StashContext cctx;
	_4800_18s queue;

public:
	SchedulerQueue() :
		ctx(now()),
		cctx(now()) { }

	TaskType peep(std::chrono::steady_clock::time_point end_time) noexcept {
		ctx.op = StashContext::Operation::peep;
		ctx.begin_key = ctx.atom_first_valid_key.load();
		ctx.end_key = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time.time_since_epoch()).count();;
		TaskType task;
		queue.next(ctx, &task);
		return task;
	}

	TaskType walk() noexcept {
		ctx.op = StashContext::Operation::walk;
		ctx.pending_floor = StashContext::IDLE;   // fresh per walk: caps first_valid at any pending leaf
		ctx.begin_key = ctx.atom_first_valid_key.load();
		ctx.end_key = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		TaskType task;
		queue.next(ctx, &task);
		return task;
	}

	void clean_checkpoint() noexcept {
		auto begin_key = ctx.atom_first_valid_key.load();
		if (begin_key < cctx.atom_first_valid_key.load()) {
			cctx.atom_first_valid_key = begin_key;
		}
		cctx.atom_last_valid_key = ctx.atom_last_valid_key.load();
	}

	void clean() noexcept {
		cctx.op = StashContext::Operation::clean;
		cctx.begin_key = cctx.atom_first_valid_key.load();
		// Reclaims up to the walk's low-water mark, with no wall-clock margin: stash's
		// sentinel frontier and pending floor make first_valid a safe boundary.
		cctx.end_key = ctx.atom_first_valid_key.load();
		TaskType task;
		queue.next(cctx, &task);
	}

	// stash's result, one to one: past_horizon when the wheel refuses the task
	// (~24 h from the walk's low-water mark), no_memory when stash could not
	// allocate before claiming a slot (no trace call under OOM); either way the
	// task never fires. stash terminates on a failure after the claim.
	// Whether add() would accept a task at time_point now; the horizon only grows.
	bool within_horizon(std::chrono::steady_clock::time_point time_point) const noexcept {
		return static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(time_point.time_since_epoch()).count()) < queue.horizon(ctx);
	}

	schedule_result add(const TaskType& task, std::chrono::steady_clock::time_point time_point) noexcept {
		return place(task, time_point);
	}

	// The same, moving the reference into the wheel (a refusal leaves `task` intact).
	schedule_result add(TaskType&& task, std::chrono::steady_clock::time_point time_point) noexcept {
		return place(std::move(task), time_point);
	}

private:
	template <typename Task>
	schedule_result place(Task&& task, std::chrono::steady_clock::time_point time_point) noexcept {
		switch (queue.add(ctx, std::chrono::duration_cast<std::chrono::nanoseconds>(time_point.time_since_epoch()).count(), std::forward<Task>(task))) {
			case stash_result::placed:
				return schedule_result::scheduled;
			case stash_result::past_horizon:
				L_DEBUG_HOOK("BaseScheduler::OVERFLOW", "BaseScheduler::" + BROWN + "Stash overflow! task dropped (scheduled past the ~24h wheel horizon)" + CLEAR_COLOR);
				L_SCHEDULER("BaseScheduler::" + BROWN + "Stash overflow!" + CLEAR_COLOR + "\n");
				return schedule_result::past_horizon;
			case stash_result::no_memory:
				break;
		}
		return schedule_result::no_memory;
	}
};


template <typename SchedulerImpl, typename ScheduledTaskImpl, ThreadPolicyType thread_policy = ThreadPolicyType::regular>
class BaseScheduler : public Thread<BaseScheduler<SchedulerImpl, ScheduledTaskImpl, thread_policy>, thread_policy> {
	using TaskType = std::shared_ptr<ScheduledTask<SchedulerImpl, ScheduledTaskImpl, thread_policy>>;

	std::mutex mtx;

	std::condition_variable wakeup_signal;
	std::atomic<std::chrono::steady_clock::time_point> atom_next_wakeup_time;
	// True while the worker is inside (or about to enter) the timed wait; add()
	// signals only then, since an awake worker re-reads the wheel on its next pass.
	std::atomic<bool> sleeping{false};

	// post(): tasks handed to the worker without touching the wheel. Posters push onto
	// a lock-free stack (one compare-exchange); the worker takes the whole stack, puts
	// the tasks back in posting order, and files them itself: into the wheel, or, for a
	// post_ordered() task, into an arrival queue. The wheel has one inserter, and the
	// order within a tick is the order the tasks were filed. The stack is on its own
	// cache line: posters write it, the worker's flags are written every pass.
	using Task = ScheduledTask<SchedulerImpl, ScheduledTaskImpl, thread_policy>;
	alignas(64) std::atomic<Task*> posted{nullptr};
	// Set by the first poster that signals the sleeping worker and cleared by the worker each
	// time it goes to sleep, so a burst of posts sends one signal.
	alignas(64) std::atomic<bool> wake_requested{false};
	bool posted_closed = false;               // worker only: finish() was seen and the stack is closed
	std::deque<TaskType> arrivals;            // worker only: filed post_ordered() tasks, oldest first
	std::uint64_t filed_count = 0;            // worker only: tasks filed so far

	// Where the stack stands once the worker has closed it: posters see it and refuse.
	static Task* closed_mark() noexcept {
		return reinterpret_cast<Task*>(static_cast<std::uintptr_t>(1));
	}

	// Whether the stack holds tasks to file (the closed mark is not one).
	bool has_posted() const noexcept {
		auto head = posted.load(std::memory_order_seq_cst);
		return head != nullptr && head != closed_mark();
	}

	// Files every posted task, oldest first. A task cancelled before it is filed is dropped
	// without an entry anywhere. `close` also shuts the stack: from then on post() refuses.
	// Returns whether it filed any.
	bool ingest(bool close = false) {
		if (close) {
			posted_closed = true;
		}
		auto head = posted.load(std::memory_order_seq_cst);
		if (head == closed_mark() || (head == nullptr && !close)) {
			return false;
		}
		head = posted.exchange(posted_closed ? closed_mark() : nullptr, std::memory_order_seq_cst);
		if (head == nullptr || head == closed_mark()) {
			return false;
		}
		// The stack is newest first: reverse it.
		Task* oldest = nullptr;
		while (head != nullptr) {
			auto next = head->post_next;
			head->post_next = oldest;
			oldest = head;
			head = next;
		}
		bool filed = false;
		while (oldest != nullptr) {
			auto next = oldest->post_next;
			TaskType task = std::move(oldest->post_hold);   // `oldest` may be freed once this goes out of scope
			oldest->post_next = nullptr;
			oldest = next;
			if (!*task) {
				continue;   // cancelled before it was filed
			}
			task->ingest_seq = ++filed_count;
			// The tick the wheel files it in (an overdue task lands at the floor of the last walk,
			// and the queue must order it as the wheel would).
			task->ingest_tick = scheduler_queue.filing_tick(task->wakeup_time);
			auto wakeup_time = task->wakeup_time;
			if (task->post_ordered) {
				try {
					arrivals.push_back(std::move(task));   // out of memory leaves `task` intact
					filed = true;
					continue;
				} catch (...) {
					// Handled as a refused wheel insert is, below: due, it runs; otherwise it is dropped.
					if (task && wakeup_time <= std::chrono::steady_clock::now()) {
						static_cast<SchedulerImpl*>(this)->operator()(task);
					}
					continue;
				}
			}
			if (scheduler_queue.add(std::move(task), wakeup_time) != schedule_result::scheduled) {
				// Past the wheel's horizon (the worker has not walked for a day) or out of memory.
				// A task that is already due runs now; any other is dropped, as add() refuses it.
				if (task && wakeup_time <= std::chrono::steady_clock::now()) {
					static_cast<SchedulerImpl*>(this)->operator()(task);
				}
				continue;
			}
			filed = true;
		}
		return filed;
	}

	// The order the wheel alone would give: by the tick a task is filed in, then by the order
	// filed. A task put on the wheel with add() has no filing tick recorded; its due tick stands.
	static bool before(const Task& a, const Task& b) noexcept {
		using Queue = SchedulerQueue<SchedulerImpl, ScheduledTaskImpl, thread_policy>;
		auto ta = a.ingest_tick ? a.ingest_tick : Queue::tick_start(a.wakeup_time);
		auto tb = b.ingest_tick ? b.ingest_tick : Queue::tick_start(b.wakeup_time);
		return ta != tb ? ta < tb : a.ingest_seq < b.ingest_seq;
	}

	// Runs what is due: the wheel's due tasks and the due head of the arrival queue, merged in
	// the order `before` gives, which is the order every task would have had on the wheel.
	// `limit` is read before the wheel is walked, so a walk that finds nothing means no wheel
	// task is due before `limit`, and the queue's tasks due before `limit` can run without
	// looking at the wheel again (the wheel is walked again every 256 of them).
	void fire_due() {
		using Queue = SchedulerQueue<SchedulerImpl, ScheduledTaskImpl, thread_policy>;
		unsigned long long limit = Queue::now();
		TaskType held = scheduler_queue.walk();
		unsigned fired_from_queue = 0;
		for (;;) {
			if (held) {
				if (!arrivals.empty() && before(*arrivals.front(), *held)) {
					TaskType task = std::move(arrivals.front());
					arrivals.pop_front();
					static_cast<SchedulerImpl*>(this)->operator()(task);
				} else {
					TaskType task = std::move(held);
					static_cast<SchedulerImpl*>(this)->operator()(task);
					held = scheduler_queue.walk();
				}
			} else if (!arrivals.empty() && arrivals.front()->ingest_tick < limit) {
				TaskType task = std::move(arrivals.front());
				arrivals.pop_front();
				static_cast<SchedulerImpl*>(this)->operator()(task);
				if (++fired_from_queue % 256 == 0) {
					limit = Queue::now();
					held = scheduler_queue.walk();
				}
			} else {
				break;
			}
		}
	}

	SchedulerQueue<SchedulerImpl, ScheduledTaskImpl, thread_policy> scheduler_queue;

	std::string _name;
	std::atomic_int ending;

protected:
	void end(int wait = 10) {
		ending = wait;

		std::lock_guard<std::mutex> lk(mtx);
		wakeup_signal.notify_all();
	}

public:
	BaseScheduler(std::string name) :
		atom_next_wakeup_time(std::chrono::steady_clock::time_point{}),
		_name(std::move(name)),
		ending(-1) {
		Thread<BaseScheduler<SchedulerImpl, ScheduledTaskImpl, thread_policy>, thread_policy>::run();
	}

	const std::string& name() const noexcept {
		return _name;
	}

	// finish() has begun: add() refuses with schedule_result::stopping.
	bool is_stopping() const noexcept {
		return ending.load() >= 0;
	}

	// Whether add() would accept a task at wakeup_time now (the 24 h horizon).
	bool within_horizon(std::chrono::steady_clock::time_point wakeup_time) const noexcept {
		return scheduler_queue.within_horizon(wakeup_time);
	}

	void operator()() {
		L_SCHEDULER("BaseScheduler::" + LIGHT_SKY_BLUE + "STARTED" + CLEAR_COLOR);

		std::unique_lock<std::mutex> lk(mtx);
		auto next_wakeup_time = atom_next_wakeup_time.load();
		lk.unlock();

		while (ending != 0) {
			// Count a finish() down, one pass at a time; while none has begun, leave `ending` (which
			// every add() and post() reads) unwritten.
			for (auto e = ending.load(std::memory_order_relaxed); e > 0 && !ending.compare_exchange_weak(e, e - 1);) {
			}

			bool pending = false;

			// File what was posted since the last pass, so the peep below sees it.
			ingest();

			// Propose a wakeup time some time in the future:
			auto now = std::chrono::steady_clock::now();
			auto wakeup_time = now + (ending < 0 ? 30s : 100ms);

			// Then figure out if there's something that needs to be acted upon sooner
			// than that wakeup time in the scheduler queue (an earlier wakeup time needed):
			TaskType task;
			L_SCHEDULER("BaseScheduler::" + DIM_GREY + "PEEPING" + CLEAR_COLOR + " - now:{}, wakeup_time:{}", std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count(), std::chrono::duration_cast<std::chrono::nanoseconds>(wakeup_time.time_since_epoch()).count());
			if ((task = scheduler_queue.peep(wakeup_time))) {
				if (task) {
					pending = true;  // flag there are still scheduled things pending.
					if (wakeup_time > task->wakeup_time) {
						wakeup_time = task->wakeup_time;
						L_SCHEDULER("BaseScheduler::" + PURPLE + "PEEP_UPDATED" + CLEAR_COLOR + " - now:{}, wakeup_time:{}  ({})", std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count(), std::chrono::duration_cast<std::chrono::nanoseconds>(wakeup_time.time_since_epoch()).count(), *task ? "valid" : "cleared");
					} else {
						L_SCHEDULER("BaseScheduler::" + DIM_GREY + "PEEPED" + CLEAR_COLOR + " - now:{}, wakeup_time:{}  ({})", std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count(), std::chrono::duration_cast<std::chrono::nanoseconds>(wakeup_time.time_since_epoch()).count(), *task ? "valid" : "cleared");
					}
				}
			}

			// The arrival queue's oldest task is due next (they are posted in due order).
			if (!arrivals.empty()) {
				pending = true;
				if (wakeup_time > arrivals.front()->wakeup_time) {
					wakeup_time = arrivals.front()->wakeup_time;
				}
			}

			SCHEDULER_TEST_HOOK("after_peep", !pending && next_wakeup_time == atom_next_wakeup_time.load());

			// Try setting the worked out wakeup time as the real next wakeup time:
			if (atom_next_wakeup_time.compare_exchange_strong(next_wakeup_time, wakeup_time)) {
				if (ending >= 0 && !pending) {
					// Nothing is due: close the stack, so a post() that passed its stopping
					// check before finish() began is refused instead of stranded, and file
					// whatever it held; those tasks are pending, so go round again.
					if (ingest(true)) {
						continue;
					}
					break;
				}
			}
			while (next_wakeup_time > wakeup_time && !atom_next_wakeup_time.compare_exchange_weak(next_wakeup_time, wakeup_time)) { }

			// Something due already means no wait: the worker walks at once (it filed the stack at the top
			// of the pass), without the mutex and without announcing `sleeping`, so producers arming tasks
			// meanwhile have no sleep to cut short and do not signal. Only a worker that may wait takes the
			// path below.
			if (wakeup_time <= now) {
				next_wakeup_time = atom_next_wakeup_time.load();
			} else {
				SCHEDULER_TEST_HOOK("sleep", wakeup_time <= now);

				// Sleep until wakeup time arrives or someone adding a task wakes us up;
				// make sure we first lock mutex so there cannot be race condition between
				// the time we load the next_wakeup_time and we actually start waiting:
				L_DEBUG_HOOK("BaseScheduler::LOOP", "BaseScheduler::" + STEEL_BLUE + "LOOP" + CLEAR_COLOR + " - now:{}, next_wakeup_time:{}", std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count(), std::chrono::duration_cast<std::chrono::nanoseconds>(atom_next_wakeup_time.load().time_since_epoch()).count());
				lk.lock();
				sleeping.store(true);
				wake_requested.store(false);
				// A task added after the peep above whose add() found nothing to lower (the
				// published wakeup time was stale, already past or earlier than the task) and
				// that saw `sleeping` still false would not signal; the worker would then sleep
				// to a target later than the task is due. The fence pairs with the one in add():
				// either add() sees `sleeping` and signals under the mutex we hold, or this
				// peep sees the task. A post() pairs the same way through `posted`: it pushes,
				// then reads `sleeping`; the worker stores `sleeping`, then reads `posted`.
				std::atomic_thread_fence(std::memory_order_seq_cst);
				// A task posted before this point is on the stack, and filing it can run task code (the
				// destructor of a task cancelled before it was filed, a task the wheel refused), which must
				// not run under `mtx` (push() takes it to signal): file it after unlocking, below, instead
				// of sleeping.
				if (!has_posted()) {
					next_wakeup_time = atom_next_wakeup_time.load();
					if (auto late = scheduler_queue.peep(next_wakeup_time)) {
						if (late->wakeup_time < next_wakeup_time) {
							next_wakeup_time = late->wakeup_time;
							auto published = atom_next_wakeup_time.load();
							while (published > next_wakeup_time && !atom_next_wakeup_time.compare_exchange_weak(published, next_wakeup_time)) { }
						}
					}
					if (!arrivals.empty() && arrivals.front()->wakeup_time < next_wakeup_time) {
						next_wakeup_time = arrivals.front()->wakeup_time;
						auto published = atom_next_wakeup_time.load();
						while (published > next_wakeup_time && !atom_next_wakeup_time.compare_exchange_weak(published, next_wakeup_time)) { }
					}
					if (next_wakeup_time > now) {
						wakeup_signal.wait_until(lk, next_wakeup_time);
					}
				}
				sleeping.store(false);
				lk.unlock();
				// File what was posted while it slept (it may be what woke it).
				ingest();
			}
			L_SCHEDULER("BaseScheduler::" + DODGER_BLUE + "WAKEUP" + CLEAR_COLOR + " - now:{}, wakeup_time:{}", std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(), std::chrono::duration_cast<std::chrono::nanoseconds>(wakeup_time.time_since_epoch()).count());

			// Walk the queue and run what is due.
			scheduler_queue.clean_checkpoint();
			fire_due();
			scheduler_queue.clean();
		}

		// Whichever way the loop ended (the countdown, or nothing pending), nothing posted may stay
		// on the stack: a task there owns itself, and nothing would ever free it. Close the stack, so
		// a post() from here on is refused, file what it held and run what is due; the rest is
		// released with the wheel, as a task still on the wheel is.
		ingest(true);
		fire_due();
	}

	// scheduled, or why not: past the wheel's horizon, out of memory, or
	// stopping (finish() began). Never throws.
	schedule_result add(const TaskType& task) noexcept {
		if (ending < 0) {
			auto wakeup_time = task->wakeup_time;
			assert(wakeup_time > std::chrono::steady_clock::time_point{});

			if (auto result = scheduler_queue.add(task, wakeup_time); result != schedule_result::scheduled) {
				return result;
			}

			// Pairs with the worker's fence before it sleeps. Everything this decides from is
			// read after the fence: either the worker's fence comes first, so this sees its
			// `sleeping` and the target it raised (and lowers it and signals), or this one
			// does, so the worker's last look at the wheel sees the task.
			std::atomic_thread_fence(std::memory_order_seq_cst);
			auto next_wakeup_time = atom_next_wakeup_time.load();
			while (next_wakeup_time > wakeup_time && !atom_next_wakeup_time.compare_exchange_weak(next_wakeup_time, wakeup_time)) { }

			auto now = std::chrono::steady_clock::now();
			if (sleeping.load() && (next_wakeup_time >= wakeup_time || next_wakeup_time <= now)) {
				std::lock_guard<std::mutex> lk(mtx);
				wakeup_signal.notify_one();
				L_SCHEDULER("BaseScheduler::" + LIGHT_GREEN + "ADDED_NOTIFY" + CLEAR_COLOR + " - now:{}, next_wakeup_time:{}, wakeup_time:{}", now, atom_next_wakeup_time.load(), wakeup_time);
			} else {
				L_SCHEDULER("BaseScheduler::" + FOREST_GREEN + "ADDED" + CLEAR_COLOR + " - now:{}, next_wakeup_time:{}, wakeup_time:{}", now, atom_next_wakeup_time.load(), wakeup_time);
			}
			return schedule_result::scheduled;
		}
		return schedule_result::stopping;
	}

	schedule_result add(const TaskType& task, std::chrono::steady_clock::time_point wakeup_time) noexcept {
		auto now = std::chrono::steady_clock::now();
		if (wakeup_time < now) {
			wakeup_time = now;
		}
		task->wakeup_time = wakeup_time;
		return add(task);
	}

	// Hands `task` to the worker, which files it into the wheel on its next pass; the
	// poster does no wheel insert and no allocation, one compare-exchange in all. The
	// task fires at `wakeup_time` (raised to `now`, the caller's own clock reading, when
	// earlier) unless it is cleared first; a task cleared before the worker files it
	// never enters the wheel. Tasks posted by one thread, or ordered by anything that
	// orders their posts, enter the wheel in that order, so within a tick they fire in
	// the order posted. Returns as add() does: `stopping` once finish() has begun (a task
	// that raced it is refused, or run if due, or released with the wheel; it is never
	// left on the stack), `past_horizon` for a time more than a
	// wheel span away. Unlike add(), an allocation failure while filing is not reported
	// to the poster: the worker runs the task now if it is due, and drops it otherwise.
	// A task is posted at most once while it is waiting to be filed. Never throws.
	template <typename T>
	schedule_result post(const std::shared_ptr<T>& task, std::chrono::steady_clock::time_point wakeup_time, std::chrono::steady_clock::time_point now) noexcept {
		// Only a distant time can be refused (the horizon only moves away).
		if (wakeup_time - now >= 1h && !scheduler_queue.within_horizon(wakeup_time)) {
			return schedule_result::past_horizon;
		}
		return push(task, wakeup_time, now);
	}

	// post() for a caller whose own posts carry nondecreasing wakeup times (a log line due a
	// fixed delay after each call, say). The worker keeps them in an arrival queue and never
	// puts them on the wheel, which costs it a fraction of an insert. They fire at the times
	// and in the order the wheel would have fired them: tasks in one tick together, by the
	// order filed, merged with the wheel's own tasks by the tick each was filed in. The worker
	// waits on the queue's oldest task. Across posters the times may fall out of order, by the
	// gap between a poster's clock reading and its push; a task that arrives behind a later
	// head then waits for that head, and wheel tasks due between the two pass it. Tasks whose
	// posts are ordered by anything that orders them, with wakeups of the poster's own clock
	// plus one fixed delay, keep due order: everything ahead of such a task was pushed before
	// it, so it is due no later than the delay after the task's push.
	template <typename T>
	schedule_result post_ordered(const std::shared_ptr<T>& task, std::chrono::steady_clock::time_point wakeup_time, std::chrono::steady_clock::time_point now) noexcept {
		Task* const node = task.get();
		node->post_ordered = true;   // before the task is published
		return push(task, wakeup_time, now);
	}

private:
	template <typename T>
	schedule_result push(const std::shared_ptr<T>& task, std::chrono::steady_clock::time_point wakeup_time, std::chrono::steady_clock::time_point now) noexcept {
		if (ending.load(std::memory_order_relaxed) >= 0) {
			return schedule_result::stopping;
		}
		SCHEDULER_PUSH_HOOK();
		if (wakeup_time < now) {
			wakeup_time = now;
		}
		Task* const node = task.get();
		node->wakeup_time = wakeup_time;
		node->post_hold = task;
		auto head = posted.load(std::memory_order_relaxed);
		do {
			if (head == closed_mark()) {
				node->post_hold.reset();
				return schedule_result::stopping;
			}
			node->post_next = head;
		} while (!posted.compare_exchange_weak(head, node, std::memory_order_seq_cst, std::memory_order_relaxed));

		// Bring the worker's wakeup forward if this task is due before it, and signal it
		// only if it sleeps past this task (or its own wakeup has passed); an awake
		// worker, or one that wakes before the task is due, files it on its next pass.
		auto next_wakeup_time = atom_next_wakeup_time.load(std::memory_order_seq_cst);
		while (next_wakeup_time > wakeup_time && !atom_next_wakeup_time.compare_exchange_weak(next_wakeup_time, wakeup_time)) { }
		// The push above and this read pair with the worker's `sleeping` store and its
		// read of `posted` (all seq_cst): either the worker sees the task, or this sees
		// `sleeping`.
		if (sleeping.load(std::memory_order_seq_cst) && (next_wakeup_time >= wakeup_time || next_wakeup_time <= now)) {
			if (!wake_requested.load(std::memory_order_seq_cst) && !wake_requested.exchange(true, std::memory_order_seq_cst)) {
				std::lock_guard<std::mutex> lk(mtx);
				wakeup_signal.notify_one();
			}
		}
		return schedule_result::scheduled;
	}
};


template <typename ScheduledTaskImpl, ThreadPolicyType thread_policy = ThreadPolicyType::regular>
class Scheduler : public BaseScheduler<Scheduler<ScheduledTaskImpl, thread_policy>, ScheduledTaskImpl, thread_policy> {
	using TaskType = std::shared_ptr<ScheduledTask<Scheduler<ScheduledTaskImpl, thread_policy>, ScheduledTaskImpl, thread_policy>>;

public:
	Scheduler(std::string name_) :
		BaseScheduler<Scheduler<ScheduledTaskImpl, thread_policy>, ScheduledTaskImpl, thread_policy>(name_) {}

	~Scheduler() noexcept {
		try {
			finish(1);
		} catch (...) {
			L_EXC("Unhandled exception in destructor");
		}
	}

	bool finish(int wait = 10) {
		BaseScheduler<Scheduler<ScheduledTaskImpl, thread_policy>, ScheduledTaskImpl, thread_policy>::end(wait);

		if (wait != 0) {
			return join(2 * wait * 100ms);
		}

		return true;
	}

	bool join(std::chrono::milliseconds timeout = 60s) {
		if (!BaseScheduler<Scheduler<ScheduledTaskImpl, thread_policy>, ScheduledTaskImpl, thread_policy>::join(timeout)) {
			return false;
		}
		return true;
	}

	void operator()(TaskType& task) {
		if (*task) {
			if (task->clear(true)) {
				L_SCHEDULER("Scheduler::" + STEEL_BLUE + "RUNNING" + CLEAR_COLOR + " - now:{}, wakeup_time:{}", std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(), std::chrono::duration_cast<std::chrono::nanoseconds>(task->wakeup_time.time_since_epoch()).count());
				task->operator()();
			}
		}
		L_SCHEDULER("Scheduler::" + BROWN + "ABORTED" + CLEAR_COLOR + " - now:{}, wakeup_time:{}", std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(), std::chrono::duration_cast<std::chrono::nanoseconds>(task->wakeup_time.time_since_epoch()).count());
	}
};


template <typename ScheduledTaskImpl, ThreadPolicyType thread_policy = ThreadPolicyType::regular>
class ThreadedScheduler : public BaseScheduler<ThreadedScheduler<ScheduledTaskImpl, thread_policy>, ScheduledTaskImpl, thread_policy> {
	using TaskType = std::shared_ptr<ScheduledTask<ThreadedScheduler<ScheduledTaskImpl, thread_policy>, ScheduledTaskImpl, thread_policy>>;

	ThreadPool<TaskType, thread_policy> thread_pool;

public:
	ThreadedScheduler(std::string name_, const char* format, size_t num_threads)  :
		BaseScheduler<ThreadedScheduler<ScheduledTaskImpl, thread_policy>, ScheduledTaskImpl, thread_policy>(name_),
		thread_pool(format, num_threads) {}

	~ThreadedScheduler() noexcept {
		try {
			finish(1);
		} catch (...) {
			L_EXC("Unhandled exception in destructor");
		}
	}

	size_t threadpool_capacity() {
		return thread_pool.threadpool_capacity();
	}

	size_t threadpool_size() {
		return thread_pool.threadpool_size();
	}

	size_t running_size() {
		return thread_pool.running_size();
	}

	size_t size() {
		return thread_pool.size();
	}

	bool finish(int wait = 10) {
		BaseScheduler<ThreadedScheduler<ScheduledTaskImpl, thread_policy>, ScheduledTaskImpl, thread_policy>::end(wait);

		thread_pool.finish();

		if (wait != 0) {
			return join(2 * wait * 100ms);
		}

		return true;
	}

	bool join(std::chrono::milliseconds timeout = 60s) {
		auto threadpool_workers = thread_pool.threadpool_workers() + 1;
		auto single_timeout = timeout / threadpool_workers;

		if (!BaseScheduler<ThreadedScheduler<ScheduledTaskImpl, thread_policy>, ScheduledTaskImpl, thread_policy>::join(single_timeout)) {
			return false;
		}
		if (!thread_pool.join(single_timeout * threadpool_workers)) {
			return false;
		}
		return true;
	}

	void operator()(TaskType& task) {
		if (*task) {
			if (task->clear(true)) {
				L_SCHEDULER("ThreadedScheduler::" + STEEL_BLUE + "RUNNING" + CLEAR_COLOR + " - now:{}, wakeup_time:{}", std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(), std::chrono::duration_cast<std::chrono::nanoseconds>(task->wakeup_time.time_since_epoch()).count());
				try {
					// The task is now dispatched but won't run until a worker picks
					// it up. A user clear() in that window can no longer win the
					// one-shot CAS, so it instead flips atom_cancelled; the worker
					// re-checks it in ScheduledTask::operator() and skips the call.
					thread_pool.enqueue(task);
				} catch (const std::logic_error&) { }
			}
		}
		L_SCHEDULER("ThreadedScheduler::" + BROWN + "ABORTED" + CLEAR_COLOR + " - now:{}, wakeup_time:{}", std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(), std::chrono::duration_cast<std::chrono::nanoseconds>(task->wakeup_time.time_since_epoch()).count());
	}
};


#ifdef L_SCHEDULER_DEFINED
#undef L_SCHEDULER_DEFINED
#undef L_SCHEDULER
#endif
