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

#include <cassert>                           // for assert
#include <chrono>                            // for std::chrono
#include <memory>                            // for std::shared_ptr, std::make_shared
#include <mutex>                             // for std::mutex
#include <optional>                          // for std::optional
#include <string>                            // for std::string
#include <tuple>                             // for std::make_tuple, std::apply
#include <unordered_map>                     // for std::unordered_map
#include <utility>                           // for std::move, std::forward

#include "callable_traits.hh"                // for callable_traits
#include "random.hh"                         // for random_time (Kronuz/random)
#include "scheduler.h"                       // for ScheduledTask, ThreadedScheduler
                                             //   (also pulls in the trace header
                                             //    that defines L_CALL / L_DEBUG_HOOK)


template <typename Key, typename Func, typename Tuple, ThreadPolicyType thread_policy>
class DebouncerTask;


template <typename Key, typename Func, typename Tuple, ThreadPolicyType thread_policy>
class Debouncer : public ThreadedScheduler<DebouncerTask<Key, Func, Tuple, thread_policy>> {
	friend DebouncerTask<Key, Func, Tuple, thread_policy>;

	// One pending task per key. A touch while it is pending only moves `target`
	// later and replaces `args`; the task re-adds itself when it fires before
	// `target`, so cancelled tasks never pile up in the wheel.
	struct Status {
		std::shared_ptr<DebouncerTask<Key, Func, Tuple, thread_policy>> task;
		std::chrono::steady_clock::time_point max_wakeup_time;
		std::chrono::steady_clock::time_point target;
		std::optional<Tuple> args;
	};

	std::mutex statuses_mtx;
	std::unordered_map<Key, Status> statuses;

	Func func;
	std::chrono::milliseconds throttle_time;
	std::chrono::milliseconds debounce_timeout;
	std::chrono::milliseconds debounce_busy_timeout;
	std::chrono::milliseconds debounce_min_force_timeout;
	std::chrono::milliseconds debounce_max_force_timeout;

	void release(const Key& key, const DebouncerTask<Key, Func, Tuple, thread_policy>* throttler);
	std::shared_ptr<DebouncerTask<Key, Func, Tuple, thread_policy>> throttle_locked(typename std::unordered_map<Key, Status>::iterator it);

public:
	Debouncer(std::string name, const char* format, size_t num_threads, Func func, std::chrono::milliseconds throttle_time, std::chrono::milliseconds debounce_timeout, std::chrono::milliseconds debounce_busy_timeout, std::chrono::milliseconds debounce_min_force_timeout, std::chrono::milliseconds debounce_max_force_timeout) :
		ThreadedScheduler<DebouncerTask<Key, Func, Tuple, thread_policy>>(name, format, num_threads),
		func(std::move(func)),
		throttle_time(throttle_time),
		debounce_timeout(debounce_timeout),
		debounce_busy_timeout(debounce_busy_timeout),
		debounce_min_force_timeout(debounce_min_force_timeout),
		debounce_max_force_timeout(debounce_max_force_timeout)
	{
		assert(throttle_time == std::chrono::milliseconds(0) || throttle_time >= debounce_timeout);
		assert(debounce_busy_timeout >= debounce_timeout);
		assert(debounce_min_force_timeout >= debounce_busy_timeout);
		assert(debounce_max_force_timeout >= debounce_busy_timeout);
		assert(debounce_min_force_timeout <= debounce_max_force_timeout);
	}

	~Debouncer() {
		// Stop and join the scheduling wheel + worker pool BEFORE this derived
		// object's members (func, statuses, throttle_time, ...) are destroyed.
		//
		// Without this, ~Debouncer would finish first, then the base
		// ~ThreadedScheduler would run finish()/join() AFTER func/statuses are
		// already gone — and any in-flight DebouncerTask touching debouncer.func /
		// throttle / statuses during shutdown would read freed memory (UAF).
		//
		// finish() is idempotent (the base destructor calls it again harmlessly:
		// end() just re-sets a flag, thread_pool.finish() is exchange-guarded, and
		// join() on already-exited threads returns immediately), so this is safe to
		// call here even though the base also calls it.
		try {
			ThreadedScheduler<DebouncerTask<Key, Func, Tuple, thread_policy>>::finish();
		} catch (...) {
			L_EXC("Unhandled exception in destructor");
		}
	}

	template <typename... Args>
	void debounce(const Key& key, Args&&... args);

	template <typename... Args>
	void delayed_debounce(std::chrono::milliseconds delay, const Key& key, Args&&... args);
};


template <typename Key, typename Func, typename Tuple, ThreadPolicyType thread_policy>
class DebouncerTask : public ScheduledTask<ThreadedScheduler<DebouncerTask<Key, Func, Tuple, thread_policy>>, DebouncerTask<Key, Func, Tuple, thread_policy>> {
	friend Debouncer<Key, Func, Tuple, thread_policy>;

	Debouncer<Key, Func, Tuple, thread_policy>& debouncer;

	bool throttler;

	Key key;

public:
	DebouncerTask(Debouncer<Key, Func, Tuple, thread_policy>& debouncer, const Key& key, bool throttler);

	void operator()();
};


template <typename Key, typename Func, typename Tuple, ThreadPolicyType thread_policy>
inline
DebouncerTask<Key, Func, Tuple, thread_policy>::DebouncerTask(Debouncer<Key, Func, Tuple, thread_policy>& debouncer, const Key& key, bool throttler) :
	debouncer(debouncer),
	throttler(throttler),
	key(key)
{
}


template <typename Key, typename Func, typename Tuple, ThreadPolicyType thread_policy>
inline void
DebouncerTask<Key, Func, Tuple, thread_policy>::operator()()
{
	L_CALL("DebouncerTask::operator()()");
	L_DEBUG_HOOK("DebouncerTask::operator()", "DebouncerTask::operator()()");

	if (throttler) {
		debouncer.release(key, this);
		return;
	}

	std::shared_ptr<DebouncerTask<Key, Func, Tuple, thread_policy>> next;
	std::optional<Tuple> args;
	{
		std::lock_guard<std::mutex> statuses_lk(debouncer.statuses_mtx);
		auto it = debouncer.statuses.find(key);
		if (it == debouncer.statuses.end() || it->second.task.get() != this) {
			return;  // superseded
		}
		auto& status = it->second;
		if (status.target > this->wakeup_time) {
			// Touched since it was armed: re-arm at the latest target, same arguments.
			next = std::make_shared<DebouncerTask<Key, Func, Tuple, thread_policy>>(debouncer, key, false);
			next->wakeup_time = status.target;
			status.task = next;
		} else {
			// Due: take the arguments and release or throttle the key in the same
			// critical section, so a touch from here on starts a new call.
			args = std::move(status.args);
			next = debouncer.throttle_locked(it);
		}
	}
	if (next) {
		debouncer.add(next);
	}
	if (args) {
		std::apply(debouncer.func, std::move(*args));
	}
}


template <typename Key, typename Func, typename Tuple, ThreadPolicyType thread_policy>
inline void
Debouncer<Key, Func, Tuple, thread_policy>::release(const Key& key, const DebouncerTask<Key, Func, Tuple, thread_policy>* throttler)
{
	std::lock_guard<std::mutex> statuses_lk(statuses_mtx);
	auto it = statuses.find(key);
	// Only the throttler still in charge releases; a touch may have replaced it.
	if (it != statuses.end() && it->second.task.get() == throttler) {
		statuses.erase(it);
	}
}


// With statuses_mtx held, after a call fired: drop the key, or turn it into a
// throttler and return the throttler task for the caller to add.
template <typename Key, typename Func, typename Tuple, ThreadPolicyType thread_policy>
inline std::shared_ptr<DebouncerTask<Key, Func, Tuple, thread_policy>>
Debouncer<Key, Func, Tuple, thread_policy>::throttle_locked(typename std::unordered_map<Key, Status>::iterator it)
{
	if (throttle_time <= debounce_timeout) {
		statuses.erase(it);
		return nullptr;
	}
	auto& status = it->second;
	status.max_wakeup_time = std::chrono::steady_clock::time_point::min();  // flag status as throttler
	auto task = std::make_shared<DebouncerTask<Key, Func, Tuple, thread_policy>>(*this, it->first, true);
	task->wakeup_time = std::chrono::steady_clock::now() + throttle_time;
	status.task = task;
	status.target = task->wakeup_time;
	return task;
}


template <typename Key, typename Func, typename Tuple, ThreadPolicyType thread_policy>
template <typename... Args>
inline void
Debouncer<Key, Func, Tuple, thread_policy>::debounce(const Key& key, Args&&... args)
{
	L_CALL("Debouncer::debounce(<key>, ...)");

	delayed_debounce(0ms, key, std::forward<Args>(args)...);
}


template <typename Key, typename Func, typename Tuple, ThreadPolicyType thread_policy>
template <typename... Args>
inline void
Debouncer<Key, Func, Tuple, thread_policy>::delayed_debounce(std::chrono::milliseconds delay, const Key& key, Args&&... args)
{
	L_CALL("Debouncer::delayed_debounce(<delay>, <key>, ...)");

	std::shared_ptr<DebouncerTask<Key, Func, Tuple, thread_policy>> task;

	{
		std::chrono::steady_clock::time_point next_wakeup_time;

		auto now = std::chrono::steady_clock::now();

		std::lock_guard<std::mutex> statuses_lk(statuses_mtx);
		auto it = statuses.find(key);

		Status* status;
		if (it == statuses.end()) {
			// status doesn't exists, initialize:
			auto& status_ref = statuses[key] = {
				nullptr,
				now + random_time(debounce_min_force_timeout, debounce_max_force_timeout) + delay,
			};
			status = &status_ref;
			next_wakeup_time = now + debounce_timeout + delay;
		} else {
			status = &(it->second);
			if (status->max_wakeup_time == std::chrono::steady_clock::time_point::min()) {
				// status exists but is a throttler (max_wakeup_time == 0), initialize:
				status->max_wakeup_time = now + random_time(debounce_min_force_timeout, debounce_max_force_timeout) + delay;
				next_wakeup_time = now + debounce_timeout + delay;
			} else {
				// status exists, so new timeout is of busy type:
				next_wakeup_time = now + debounce_busy_timeout + delay;
			}
		}

		if (next_wakeup_time > status->max_wakeup_time) {
			next_wakeup_time = status->max_wakeup_time;
		}

		if (status->task) {
			// Never fire earlier than the pending task or target; the latest arguments win.
			if (next_wakeup_time < status->target) {
				next_wakeup_time = status->target;
			}
			if (!status->task->throttler) {
				// A call is pending: move its target, and it re-arms itself.
				status->target = next_wakeup_time;
				status->args.emplace(std::forward<Args>(args)...);
				return;
			}
			status->task->clear();
		}
		task = std::make_shared<DebouncerTask<Key, Func, Tuple, thread_policy>>(*this, key, false);
		task->wakeup_time = next_wakeup_time;
		status->task = task;
		status->target = next_wakeup_time;
		status->args.emplace(std::forward<Args>(args)...);
	}

	this->add(task);
}


template <typename Key, ThreadPolicyType thread_policy = ThreadPolicyType::regular, typename Func>
inline auto
make_debouncer(std::string name, const char* format, size_t num_threads, Func func, std::chrono::milliseconds throttle_time, std::chrono::milliseconds debounce_timeout, std::chrono::milliseconds debounce_busy_timeout, std::chrono::milliseconds debounce_min_force_timeout, std::chrono::milliseconds debounce_max_force_timeout)
{
	return Debouncer<Key, decltype(func), typename callable_traits<decltype(func)>::arguments_type, thread_policy>(name, format, num_threads, func, throttle_time, debounce_timeout, debounce_busy_timeout, debounce_min_force_timeout, debounce_max_force_timeout);
}

template <typename Key, ThreadPolicyType thread_policy = ThreadPolicyType::regular, typename Func>
inline auto
make_unique_debouncer(std::string name, const char* format, size_t num_threads, Func func, std::chrono::milliseconds throttle_time, std::chrono::milliseconds debounce_timeout, std::chrono::milliseconds debounce_busy_timeout, std::chrono::milliseconds debounce_min_force_timeout, std::chrono::milliseconds debounce_max_force_timeout)
{
	return std::make_unique<Debouncer<Key, decltype(func), typename callable_traits<decltype(func)>::arguments_type, thread_policy>>(name, format, num_threads, func, throttle_time, debounce_timeout, debounce_busy_timeout, debounce_min_force_timeout, debounce_max_force_timeout);
}


template <typename Key, ThreadPolicyType thread_policy = ThreadPolicyType::regular, typename Func>
inline auto
make_shared_debouncer(std::string name, const char* format, size_t num_threads, Func func, std::chrono::milliseconds throttle_time, std::chrono::milliseconds debounce_timeout, std::chrono::milliseconds debounce_busy_timeout, std::chrono::milliseconds debounce_min_force_timeout, std::chrono::milliseconds debounce_max_force_timeout)
{
	return std::make_shared<Debouncer<Key, decltype(func), typename callable_traits<decltype(func)>::arguments_type, thread_policy>>(name, format, num_threads, func, throttle_time, debounce_timeout, debounce_busy_timeout, debounce_min_force_timeout, debounce_max_force_timeout);
}
