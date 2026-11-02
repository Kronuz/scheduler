cmake_minimum_required(VERSION 3.14)
project(scheduler LANGUAGES CXX)

include(FetchContent)

# --- Dependencies ---
# The scheduler is built on three already-extracted standalone libraries:
#   stash       — the lock-free hierarchical slot store (the timer-wheel primitive)
#   threadpool  — Thread<Impl,policy> + ThreadPolicyType + ThreadPool (+ TaskQueue)
# Both are public (consumers see stash.h / thread.hh / threadpool.hh on their
# include path), pinned by commit.
FetchContent_Declare(
  stash
  GIT_REPOSITORY https://github.com/Kronuz/stash.git
  GIT_TAG        3bfc455d275d84175da779ec934141ce6ab99418
)
FetchContent_Declare(
  threadpool
  GIT_REPOSITORY https://github.com/Kronuz/threadpool.git
  GIT_TAG        287b87740d667419e632799b34566db639dd023e
)
FetchContent_MakeAvailable(stash threadpool)
# random_time (the debouncer's force window) comes from the random library; reuse
# a consumer's copy when it already made one available.
if(NOT TARGET random::random)
  FetchContent_Declare(
    random
    GIT_REPOSITORY https://github.com/Kronuz/random.git
    GIT_TAG        3956907a8a839fa3c44d69ffcb2cf95cbd0afc0f
  )
  FetchContent_MakeAvailable(random)
endif()

# --- Library target ---
# Header-only. The source dir is exposed so scheduler.h / debouncer.h / the bundled
# scheduler_trace.h are on the include path.
add_library(scheduler INTERFACE)
target_include_directories(scheduler INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(scheduler INTERFACE cxx_std_20)
target_link_libraries(scheduler INTERFACE stash threadpool::threadpool random::random)
# Alias so consumers can link `scheduler::scheduler` whether they add this via
# add_subdirectory or FetchContent.
add_library(scheduler::scheduler ALIAS scheduler)

# Only build the test when this repo is the top-level project, so consumers
# (e.g. via FetchContent) don't build our tests.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_testing()
  add_executable(scheduler_test test/test.cc)
  target_link_libraries(scheduler_test PRIVATE scheduler)
  add_test(NAME scheduler COMMAND scheduler_test)

  # Lost-task soak: a short smoke run under ctest; run it with a large add count (and
  # under TSan) to soak, e.g. `scheduler_soak 100000000 1 2 1`.
  add_executable(scheduler_soak test/soak.cc)
  target_link_libraries(scheduler_soak PRIVATE scheduler)
  add_test(NAME scheduler_soak COMMAND scheduler_soak 300000 1 2 0)
  add_test(NAME scheduler_soak_burst COMMAND scheduler_soak 60000 1 2 1)
  # Three producers on one wheel: stash's concurrent insert under the sanitizers.
  add_test(NAME scheduler_soak_producers COMMAND scheduler_soak 150000 3 2 0)

  # post(): the lock-free hand-off from producer threads to the worker (order, cancel,
  # wake, many producers, a finish() that races the posters).
  add_executable(scheduler_post_test test/post.cc)
  target_link_libraries(scheduler_post_test PRIVATE scheduler)
  add_test(NAME scheduler_post COMMAND scheduler_post_test)

  # The size of ScheduledTask (what every task a caller makes pays for) and the stages of its hand-off fields.
  add_executable(scheduler_task_size_test test/task_size.cc)
  target_link_libraries(scheduler_task_size_test PRIVATE scheduler)
  add_test(NAME scheduler_task_size COMMAND scheduler_task_size_test)

  # add()'s side of the sleep handshake, read from scheduler.h, against a store-buffer model of
  # the worker's pass in every interleaving (no hardware run can show a fence placed too late).
  add_executable(scheduler_handshake_model test/handshake_model.cc)
  add_test(NAME scheduler_handshake_model COMMAND scheduler_handshake_model "${CMAKE_CURRENT_SOURCE_DIR}/scheduler.h")

  # Allocation failure (no_memory). It replaces global operator new and delete,
  # which TSan's static runtime also defines (a link clash), so TSan builds skip
  # it; CI passes TSan through the per-config and linker flags.
  string(TOUPPER "${CMAKE_BUILD_TYPE}" SCHEDULER_BUILD_TYPE)
  set(SCHEDULER_ALL_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${SCHEDULER_BUILD_TYPE}} ${CMAKE_EXE_LINKER_FLAGS}")
  if(NOT SCHEDULER_ALL_FLAGS MATCHES "-fsanitize=thread")
    add_executable(scheduler_no_memory_test test/no_memory.cc)
    target_link_libraries(scheduler_no_memory_test PRIVATE scheduler)
    add_test(NAME scheduler_no_memory COMMAND scheduler_no_memory_test)
  endif()

  # A runnable tour of the library (not a test): build & run ./build/scheduler_demo
  add_executable(scheduler_demo examples/demo.cc)
  target_link_libraries(scheduler_demo PRIVATE scheduler)

  # Cancel churn on a debouncer (manual benchmark, not a test). Opt-in: it replaces
  # global operator new and delete, which sanitizer runtimes also define.
  option(SCHEDULER_BENCH "Build bench/bench_churn.cc" OFF)
  if(SCHEDULER_BENCH)
    add_executable(scheduler_bench_churn bench/bench_churn.cc)
    target_link_libraries(scheduler_bench_churn PRIVATE scheduler)
    # What a producer pays to arm a task: add(), post() and post_ordered(); --compare adds a
    # priority_queue + mutex + condition_variable timer queue, after a same-results check that
    # --check runs alone (the CTest test below).
    add_executable(scheduler_bench_post bench/bench_post.cc)
    target_link_libraries(scheduler_bench_post PRIVATE scheduler)
    add_test(NAME scheduler_bench_post_check COMMAND scheduler_bench_post --check)
  endif()
endif()
