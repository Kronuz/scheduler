cmake_minimum_required(VERSION 3.14)
project(scheduler LANGUAGES CXX)

include(FetchContent)

# --- Dependencies ---
# The scheduler is built on three already-extracted standalone libraries:
#   stash       — the lock-free hierarchical slot store (the timer-wheel primitive)
#   threadpool  — Thread<Impl,policy> + ThreadPolicyType + ThreadPool (+ TaskQueue)
# Both are public (consumers see stash.h / thread.hh / threadpool.hh on their
# include path), tracking their default-branch tips.
FetchContent_Declare(
  stash
  GIT_REPOSITORY https://github.com/Kronuz/stash.git
  GIT_TAG        a080a1ed77879155bba288bfa30548c3f16d1832
)
FetchContent_Declare(
  threadpool
  GIT_REPOSITORY https://github.com/Kronuz/threadpool.git
  GIT_TAG        628679a2e27b563cf2709a6f35f98f1657baf7b3
)
FetchContent_MakeAvailable(stash threadpool)
# random_time (the debouncer's force window) comes from the random library; reuse
# a consumer's copy when it already made one available.
if(NOT TARGET random::random)
  FetchContent_Declare(
    random
    GIT_REPOSITORY https://github.com/Kronuz/random.git
    GIT_TAG        6487253c9e7a4855ccbea36f7a66cea5fef7d381
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
  endif()
endif()
