cmake_minimum_required(VERSION 3.14)
project(scheduler LANGUAGES CXX)

include(FetchContent)

# --- Dependencies ---
# The scheduler is built on two already-extracted standalone libraries:
#   stash       — the lock-free hierarchical slot store (the timer-wheel primitive)
#   threadpool  — Thread<Impl,policy> + ThreadPolicyType + ThreadPool (+ TaskQueue)
# Both are public (consumers see stash.h / thread.hh / threadpool.hh on their
# include path), pinned to the SHAs the scheduler was extracted against.
FetchContent_Declare(
  stash
  GIT_REPOSITORY https://github.com/Kronuz/stash.git
  GIT_TAG        b3794af00f72a54510440f89852f9621daf7535b
)
FetchContent_Declare(
  threadpool
  GIT_REPOSITORY https://github.com/Kronuz/threadpool.git
  GIT_TAG        2c48981c9a0f2c3391d337ca552a3ba8f78b1951
)
FetchContent_MakeAvailable(stash threadpool)

# --- Library target ---
# The scheduler headers are header-only, but the debouncer needs the three
# uniform-random helpers in random.cc, so the target is a small STATIC library
# carrying that one translation unit. Consumers that only use scheduler.h still
# link it harmlessly. The source dir is exposed so scheduler.h / debouncer.h /
# the bundled scheduler_trace.h are on the include path.
add_library(scheduler STATIC random.cc)
target_include_directories(scheduler PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(scheduler PUBLIC cxx_std_20)
target_link_libraries(scheduler PUBLIC stash threadpool::threadpool)
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
endif()
