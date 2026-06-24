# colored_trace example

A runnable demonstration that `scheduler`'s tracing and coloring are fully
injectable from outside the library, with no edit to `scheduler.h` or
`debouncer.h`.

`scheduler.h` is heavily instrumented, but every trace line flows through four
macros that are no-ops by default: `L_SCHEDULER`, `L_DEBUG_HOOK`, `L_EXC`, and
`L_CALL` (the last used by the debouncer). With the default no-op header those
macros expand to nothing, so the color identifiers `scheduler.h` names inside
their arguments (`BROWN`, `CLEAR_COLOR`, `LIGHT_SKY_BLUE`, `DIM_GREY`, `PURPLE`,
`STEEL_BLUE`, `DODGER_BLUE`, `LIGHT_GREEN`, `FOREST_GREEN`) are discarded by the
preprocessor and never compiled.

This example provides real versions of all four macros plus all nine color
identifiers in `trace.h`, so:

- the trace strings inside `scheduler.h` concatenate the colors into their
  messages, and
- the `L_*` macros render the runtime-built format strings and print them, so
  every `{}` is substituted.

`main.cc` then runs a small inline `Scheduler`, schedules two tasks out of
wakeup order, and lets them fire. You see colored trace lines for the scheduler
loop (`STARTED`, `PEEPING`, `LOOP`, `ADDED_NOTIFY`, `WAKEUP`, `RUNNING`,
`ABORTED`), and the two tasks fire in wakeup order.

## Build & run

The deps (`stash`, `threadpool`) are normally fetched by CMake. The simplest way
to build this example is from a top-level CMake build of the repo, which has
already fetched them into `build/_deps`:

```sh
# from the repo root, after `cmake -B build`
STASH=build/_deps/stash-src
TP=build/_deps/threadpool-src
TPCFG=build/_deps/threadpool-build   # generated config.h lives here

c++ -std=c++20 \
  -Iexamples/colored_trace -I. -I"$STASH" -I"$TP" -I"$TPCFG" \
  -DSCHEDULER_TRACE_HEADER='"trace.h"' \
  examples/colored_trace/main.cc random.cc "$TP/thread.cc" \
  -o /tmp/sched_demo && /tmp/sched_demo
```

`-DSCHEDULER_TRACE_HEADER='"trace.h"'` tells `scheduler.h` to include `trace.h`
instead of the bundled no-op `scheduler_trace.h`. C++20 is required throughout.

Drop the `-DSCHEDULER_TRACE_HEADER=...` flag and the exact same `scheduler.h`
produces no trace and no color at all (and the color identifiers are never
referenced).

## A note on argument types

`scheduler.h` passes a grab-bag of types into the trace `{}` slots: plain
integers (nanosecond counts) on most lines, but raw `steady_clock::time_point`s
on the `ADDED` / `ADDED_NOTIFY` lines. libc++'s `std::format` cannot format a
`steady_clock::time_point`, so `trace.h`'s renderer stringifies each argument
itself (specializing on `time_point`) and substitutes `{}` by hand rather than
forwarding raw types to `std::vformat` (which would be a hard compile error).

A real consumer such as Xapiand points the same four macros at its own logger and
color palette (`log.h` / `colors.h`) instead, whose formatter already knows how
to render its own types, recovering the colored debug tracing it relies on
without modifying `scheduler.h`.
