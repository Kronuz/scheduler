/*
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


// #define L_SCHEDULER L_PRINT


#ifndef L_SCHEDULER
#define L_SCHEDULER_DEFINED
#define L_SCHEDULER L_NOTHING
#endif


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

private:
	/*                               <  _Tp         _Size  _CurrentKey     _Div        _Mod    _Ring >*/
	using _tasks =        StashValues<TaskType,     10ULL,  &now>;
	using _50_1ms =       StashSlots<_tasks,        10ULL,  &now,        1ULL * MS,    50ULL,  false>;
	using _10_50ms =      StashSlots<_50_1ms,       10ULL,  &now,       50ULL * MS,    10ULL,  false>;
	using _36_500ms =     StashSlots<_10_50ms,      12ULL,  &now,      500ULL * MS,    36ULL,  false>;
	using _4800_18s =     StashSlots<_36_500ms,   4800ULL,  &now,    18000ULL * MS,  4800ULL,  true>;

	StashContext ctx;
	StashContext cctx;
	_4800_18s queue;

public:
	SchedulerQueue() :
		ctx(now()),
		cctx(now()) {}

	TaskType peep(std::chrono::steady_clock::time_point end_time) {
		ctx.op = StashContext::Operation::peep;
		ctx.begin_key = ctx.atom_first_valid_key.load();
		ctx.end_key = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time.time_since_epoch()).count();;
		TaskType task;
		queue.next(ctx, &task);
		return task;
	}

	TaskType walk() {
		ctx.op = StashContext::Operation::walk;
		ctx.begin_key = ctx.atom_first_valid_key.load();
		ctx.end_key = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		TaskType task;
		queue.next(ctx, &task);
		return task;
	}

	void clean_checkpoint() {
		auto begin_key = ctx.atom_first_valid_key.load();
		if (begin_key < cctx.atom_first_valid_key.load()) {
			cctx.atom_first_valid_key = begin_key;
		}
		cctx.atom_last_valid_key = ctx.atom_last_valid_key.load();
	}

	void clean() {
		cctx.op = StashContext::Operation::clean;
		cctx.begin_key = cctx.atom_first_valid_key.load();
		cctx.end_key = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch() - 1min).count();
		TaskType task;
		queue.next(cctx, &task);
	}

	void add(const TaskType& task, std::chrono::steady_clock::time_point time_point) {
		try {
			queue.add(ctx, std::chrono::duration_cast<std::chrono::nanoseconds>(time_point.time_since_epoch()).count(), task);
		} catch (const std::out_of_range&) {
			// SILENT-DROP WARNING: the task is scheduled beyond the wheel's horizon
			// (~24h, the span of the outermost StashSlots ring) and is being
			// DISCARDED here. add() returns void and is on the hot path with many
			// call sites (BaseScheduler::add, Debouncer), so widening it to report
			// failure would be an API break that ripples through all of them; we
			// keep the signature and instead surface the drop through the injectable
			// trace hooks. Consumers that care should override L_DEBUG_HOOK (or the
			// SCHEDULER_TRACE_HEADER) to make this audible — by default it is a
			// no-op. The task's callback will simply never fire.
			L_DEBUG_HOOK("BaseScheduler::OVERFLOW", "BaseScheduler::" + BROWN + "Stash overflow! task dropped (scheduled past the ~24h wheel horizon)" + CLEAR_COLOR);
			L_SCHEDULER("BaseScheduler::" + BROWN + "Stash overflow!" + CLEAR_COLOR + "\n");
		}
	}
};


template <typename SchedulerImpl, typename ScheduledTaskImpl, ThreadPolicyType thread_policy = ThreadPolicyType::regular>
class BaseScheduler : public Thread<BaseScheduler<SchedulerImpl, ScheduledTaskImpl, thread_policy>, thread_policy> {
	using TaskType = std::shared_ptr<ScheduledTask<SchedulerImpl, ScheduledTaskImpl, thread_policy>>;

	std::mutex mtx;

	std::condition_variable wakeup_signal;
	std::atomic<std::chrono::steady_clock::time_point> atom_next_wakeup_time;

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

	void operator()() {
		L_SCHEDULER("BaseScheduler::" + LIGHT_SKY_BLUE + "STARTED" + CLEAR_COLOR);

		std::unique_lock<std::mutex> lk(mtx);
		auto next_wakeup_time = atom_next_wakeup_time.load();
		lk.unlock();

		while (ending != 0) {
			if (--ending < 0) {
				ending = -1;
			}

			bool pending = false;

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

			// Try setting the worked out wakeup time as the real next wakeup time:
			if (atom_next_wakeup_time.compare_exchange_strong(next_wakeup_time, wakeup_time)) {
				if (ending >= 0 && !pending) {
					break;
				}
			}
			while (next_wakeup_time > wakeup_time && !atom_next_wakeup_time.compare_exchange_weak(next_wakeup_time, wakeup_time)) { }

			// Sleep until wakeup time arrives or someone adding a task wakes us up;
			// make sure we first lock mutex so there cannot be race condition between
			// the time we load the next_wakeup_time and we actually start waiting:
			L_DEBUG_HOOK("BaseScheduler::LOOP", "BaseScheduler::" + STEEL_BLUE + "LOOP" + CLEAR_COLOR + " - now:{}, next_wakeup_time:{}", std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count(), std::chrono::duration_cast<std::chrono::nanoseconds>(atom_next_wakeup_time.load().time_since_epoch()).count());
			lk.lock();
			next_wakeup_time = atom_next_wakeup_time.load();
			if (next_wakeup_time > now) {
				wakeup_signal.wait_until(lk, next_wakeup_time);
			}
			lk.unlock();
			L_SCHEDULER("BaseScheduler::" + DODGER_BLUE + "WAKEUP" + CLEAR_COLOR + " - now:{}, wakeup_time:{}", std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(), std::chrono::duration_cast<std::chrono::nanoseconds>(wakeup_time.time_since_epoch()).count());

			// Start walking the queue and running still pending tasks.
			scheduler_queue.clean_checkpoint();
			while ((task = scheduler_queue.walk())) {
				if (task) {
					static_cast<SchedulerImpl*>(this)->operator()(task);
				}
			}
			scheduler_queue.clean();
		}
	}

	void add(const TaskType& task) {
		if (ending < 0) {
			auto wakeup_time = task->wakeup_time;
			assert(wakeup_time > std::chrono::steady_clock::time_point{});

			scheduler_queue.add(task, wakeup_time);

			auto next_wakeup_time = atom_next_wakeup_time.load();
			while (next_wakeup_time > wakeup_time && !atom_next_wakeup_time.compare_exchange_weak(next_wakeup_time, wakeup_time)) { }

			auto now = std::chrono::steady_clock::now();
			if (next_wakeup_time >= wakeup_time || next_wakeup_time <= now) {
				std::lock_guard<std::mutex> lk(mtx);
				wakeup_signal.notify_one();
				L_SCHEDULER("BaseScheduler::" + LIGHT_GREEN + "ADDED_NOTIFY" + CLEAR_COLOR + " - now:{}, next_wakeup_time:{}, wakeup_time:{}", now, atom_next_wakeup_time.load(), wakeup_time);
			} else {
				L_SCHEDULER("BaseScheduler::" + FOREST_GREEN + "ADDED" + CLEAR_COLOR + " - now:{}, next_wakeup_time:{}, wakeup_time:{}", now, atom_next_wakeup_time.load(), wakeup_time);
			}
		}
	}

	void add(const TaskType& task, std::chrono::steady_clock::time_point wakeup_time) {
		auto now = std::chrono::steady_clock::now();
		if (wakeup_time < now) {
			wakeup_time = now;
		}
		task->wakeup_time = wakeup_time;
		add(task);
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
