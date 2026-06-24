/*
 * Example trace header for `scheduler`.
 *
 * Including this (via -DSCHEDULER_TRACE_HEADER='"trace.h"') turns scheduler.h /
 * debouncer.h's no-op tracing hooks into real, colored, std::format-rendered
 * output, demonstrating that the colored debug tracing Xapiand relies on is
 * fully recoverable by a consumer without modifying scheduler.h. A real consumer
 * (like Xapiand) would point these hooks at its own logger and color palette
 * (log.h / colors.h) instead.
 *
 * It also forces the build to actually compile the color identifiers
 * (BROWN, CLEAR_COLOR, LIGHT_SKY_BLUE, ...) that scheduler.h names inside its
 * L_SCHEDULER(...) / L_DEBUG_HOOK(...) arguments. With the default no-op header
 * those macros expand to nothing and the identifiers are never compiled; here
 * the macros expand to real calls, so the identifiers must exist. This header
 * supplies all nine.
 *
 * Requires C++20 (for std::format / std::vformat).
 */

#pragma once

#include <chrono>
#include <cstdio>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// --- ANSI color constants used inside scheduler.h's trace messages ---
// scheduler.h builds trace strings like "BaseScheduler::" + DODGER_BLUE +
// "WAKEUP" + CLEAR_COLOR so each token must concatenate with string literals;
// std::string does. These are the exact nine names scheduler.h references.
inline const std::string CLEAR_COLOR    = "\033[0m";
inline const std::string BROWN          = "\033[33m";
inline const std::string LIGHT_SKY_BLUE = "\033[94m";
inline const std::string DIM_GREY       = "\033[90m";
inline const std::string PURPLE         = "\033[35m";
inline const std::string STEEL_BLUE     = "\033[38;5;67m";
inline const std::string DODGER_BLUE    = "\033[38;5;33m";
inline const std::string LIGHT_GREEN    = "\033[92m";
inline const std::string FOREST_GREEN   = "\033[32m";

// --- logging macros ---
// The format string is built at runtime (from the color concatenations above)
// and scheduler.h passes a grab-bag of argument types into the `{}` slots:
// plain integers (nanosecond counts) on most lines, but also raw
// steady_clock::time_points on the ADDED / ADDED_NOTIFY lines. libc++'s
// std::format cannot format a steady_clock::time_point, so instead of forwarding
// the raw types to std::vformat (a hard compile error) we stringify each
// argument ourselves first, then substitute the `{}` placeholders by hand. This
// keeps the demo robust to whatever scheduler.h hands a trace point. A real
// consumer (Xapiand) plugs its own logger in here, which knows how to render its
// own types.
namespace scheduler_example {

template <typename T>
inline std::string to_text(const T& v) {
	if constexpr (std::is_same_v<T, std::chrono::steady_clock::time_point>) {
		return std::to_string(
			std::chrono::duration_cast<std::chrono::nanoseconds>(v.time_since_epoch()).count());
	} else {
		std::ostringstream os;
		os << v;
		return os.str();
	}
}

inline std::string render(std::string_view f) { return std::string(f); }

template <typename... Args>
inline std::string render(std::string_view f, Args&&... args) {
	std::vector<std::string> parts{to_text(args)...};
	std::string out;
	out.reserve(f.size());
	std::size_t next = 0;
	for (std::size_t i = 0; i < f.size(); ++i) {
		if (i + 1 < f.size() && f[i] == '{' && f[i + 1] == '}') {
			out += (next < parts.size()) ? parts[next++] : "{}";
			++i;
		} else {
			out += f[i];
		}
	}
	return out;
}
}  // namespace scheduler_example

// scheduler.h sets L_SCHEDULER to L_NOTHING unless it's already defined, so we
// define it here to route to the renderer. L_NOTHING stays a true no-op.
// scheduler.h includes stash.h (which pulls in stash's own no-op stubs for
// L_DEBUG_HOOK / L_EXC) before it includes this trace header, so #undef them
// first to replace the no-ops cleanly rather than redefine-warn.
#ifndef L_NOTHING
#define L_NOTHING(...)
#endif
#undef L_SCHEDULER
#undef L_DEBUG_HOOK
#undef L_EXC
#undef L_CALL
#define L_SCHEDULER(...)         std::puts((scheduler_example::render(__VA_ARGS__) + "\033[0m").c_str())
#define L_DEBUG_HOOK(label, ...) std::puts((scheduler_example::render(__VA_ARGS__) + "\033[0m").c_str())
#define L_EXC(...)               std::puts(scheduler_example::render(__VA_ARGS__).c_str())
#define L_CALL(...)              std::puts((scheduler_example::render(__VA_ARGS__) + "\033[0m").c_str())
