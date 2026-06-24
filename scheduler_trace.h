/*
 * Default (no-op) tracing hooks for the standalone `scheduler` library.
 *
 * `scheduler.h` and `debouncer.h` instrument themselves through four logging
 * macros that are no-ops by default, so the timer wheel builds with zero
 * dependency on any logging or color header, and tracing has no runtime cost:
 *
 *   - L_SCHEDULER(...)   — the wheel/scheduler's own trace channel. `scheduler.h`
 *                          sets it to L_NOTHING unless it's already defined.
 *   - L_DEBUG_HOOK(...)  — a labelled debug hook (first arg is a label string,
 *                          then a format + args).
 *   - L_EXC(...)         — logs an exception swallowed in a destructor.
 *   - L_CALL(...)        — a call-trace channel used by the debouncer.
 *
 * To restore traced, colored output (the way Xapiand uses it), provide your own
 * versions. Two ways:
 *
 *   1. Define SCHEDULER_TRACE_HEADER to the path of a header that defines them,
 *      e.g.
 *        c++ -DSCHEDULER_TRACE_HEADER='"my_trace.h"' ...
 *      `scheduler.h` will include that instead of this file.
 *
 *   2. Define the macros directly before including `scheduler.h`.
 *
 * Each macro is `#ifndef`-guarded, so defining any subset is fine; the rest fall
 * back to the no-op defaults here. See examples/colored_trace/ for a complete,
 * runnable override that produces colored, std::format-rendered trace lines.
 *
 * A note on the color identifiers (BROWN, CLEAR_COLOR, LIGHT_SKY_BLUE, ...):
 * `scheduler.h` references them only inside the arguments of L_SCHEDULER(...) /
 * L_DEBUG_HOOK(...). Because those macros expand to nothing by default, the
 * preprocessor discards their arguments and the color identifiers are never
 * compiled — so no color stubs are needed for the default no-op build. A trace
 * header that turns the macros into real, string-building calls must also define
 * those color identifiers (as `std::string`s that concatenate with `+`). See
 * examples/colored_trace/trace.h.
 */

#pragma once

// L_SCHEDULER is set to L_NOTHING by scheduler.h unless already defined.
#ifndef L_NOTHING
#define L_NOTHING(...)
#endif

// Labelled debug hook: first argument is a label, then a format + args.
#ifndef L_DEBUG_HOOK
#define L_DEBUG_HOOK(...)
#endif

// Logs an exception swallowed in a Scheduler / ThreadedScheduler destructor.
#ifndef L_EXC
#define L_EXC(...)
#endif

// Call-trace channel used by the debouncer.
#ifndef L_CALL
#define L_CALL(...)
#endif
