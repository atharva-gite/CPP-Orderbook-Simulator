cmake_minimum_required(VERSION 3.25)

project(lob LANGUAGES CXX)

include(CTest)
include(FetchContent)

option(LOB_ENABLE_ASAN "Enable Address/Undefined Behavior Sanitizer" OFF)
option(LOB_ENABLE_TSAN "Enable Thread Sanitizer" OFF)
option(LOB_ENABLE_BENCH "Enable Google Benchmark fetch and build" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Warnings as errors in Debug
if(NOT MSVC)
  add_compile_options($<$<CONFIG:Debug>:-Werror>)
else()
  add_compile_options($<$<CONFIG:Debug>:/WX>)
endif()

# Release flags
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -march=native -DNDEBUG")

if(LOB_ENABLE_ASAN)
  if(NOT MSVC)
    add_compile_options(-fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer)
  endif()
endif()

if(LOB_ENABLE_TSAN)
  if(NOT MSVC)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread -fno-omit-frame-pointer)
  endif()
endif()

# Fetch Catch2 for tests
FetchContent_Declare(
  catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.4.0
)
FetchContent_MakeAvailable(catch2)

# Fetch Google Benchmark for benchmarks (optional)
if(LOB_ENABLE_BENCH)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_DOWNLOAD_DEPENDENCIES ON)
  FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.8.5
  )
  FetchContent_MakeAvailable(benchmark)
endif()

# Library target
add_library(lob STATIC src/lob.cpp src/order_pool.cpp src/order_book.cpp src/simulator.cpp)
add_library(matching STATIC src/matching_engine.cpp)
target_include_directories(matching PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(matching PRIVATE lob)
target_include_directories(lob PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lob PUBLIC cxx_std_20)

# Apply sanitizer flags to targets if enabled
function(lob_apply_sanitizers tgt)
  if(LOB_ENABLE_ASAN)
    if(NOT MSVC)
      target_compile_options(${tgt} PRIVATE -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer)
      target_link_options(${tgt} PRIVATE -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer)
    endif()
  endif()
  if(LOB_ENABLE_TSAN)
    if(NOT MSVC)
      target_compile_options(${tgt} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
      target_link_options(${tgt} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
      target_compile_definitions(${tgt} PRIVATE THREAD_SANITIZER=1)
    endif()
  endif()
endfunction()

if(LOB_ENABLE_ASAN)
  if(NOT MSVC)
    target_compile_options(lob PRIVATE -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(lob PRIVATE -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer)
  endif()
endif()

if(LOB_ENABLE_TSAN)
  if(NOT MSVC)
    target_compile_options(lob PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(lob PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
  endif()
endif()

find_package(Threads REQUIRED)

# Tests
add_executable(lob_tests tests/test_lob.cpp)
target_link_libraries(lob_tests PRIVATE lob Catch2::Catch2WithMain)
lob_apply_sanitizers(lob_tests)
add_test(NAME lob_tests COMMAND lob_tests)

add_executable(lob_order_tests tests/test_order_pool.cpp tests/test_order_book.cpp)
target_link_libraries(lob_order_tests PRIVATE lob Catch2::Catch2WithMain)
lob_apply_sanitizers(lob_order_tests)
add_test(NAME lob_order_tests COMMAND lob_order_tests)

add_executable(lob_matching_tests tests/test_matching.cpp)
target_link_libraries(lob_matching_tests PRIVATE lob matching Catch2::Catch2WithMain)
lob_apply_sanitizers(lob_matching_tests)
lob_apply_sanitizers(matching)
add_test(NAME lob_matching_tests COMMAND lob_matching_tests)

add_executable(lob_concurrent_tests tests/test_concurrent.cpp)
target_link_libraries(lob_concurrent_tests PRIVATE lob Catch2::Catch2WithMain Threads::Threads)
lob_apply_sanitizers(lob_concurrent_tests)
add_test(NAME lob_concurrent_tests COMMAND lob_concurrent_tests)

add_library(mdnet STATIC net/md_publisher.cpp net/md_receiver.cpp net/thread_pin.cpp)
target_include_directories(mdnet PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/net
  ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(mdnet PUBLIC lob Threads::Threads)
target_compile_features(mdnet PUBLIC cxx_std_20)
lob_apply_sanitizers(mdnet)

add_executable(lob_md_tests tests/test_md_multicast.cpp)
target_link_libraries(lob_md_tests PRIVATE mdnet Catch2::Catch2WithMain Threads::Threads)
lob_apply_sanitizers(lob_md_tests)
add_test(NAME lob_md_tests COMMAND lob_md_tests)

add_executable(lob_contention_bench bench/bench_contention.cpp)
target_link_libraries(lob_contention_bench PRIVATE lob Threads::Threads)
target_compile_features(lob_contention_bench PRIVATE cxx_std_20)
lob_apply_sanitizers(lob_contention_bench)

add_executable(lob_demo demo/lob_demo.cpp)
target_link_libraries(lob_demo PRIVATE lob matching)
target_compile_features(lob_demo PRIVATE cxx_std_20)
lob_apply_sanitizers(lob_demo)

add_executable(lob_profile bench/profile_pipeline.cpp)
target_link_libraries(lob_profile PRIVATE lob matching mdnet Threads::Threads)
target_compile_features(lob_profile PRIVATE cxx_std_20)
lob_apply_sanitizers(lob_profile)

# Benchmarks
if(LOB_ENABLE_BENCH)
  add_executable(lob_bench bench/bench_lob.cpp)
  target_link_libraries(lob_bench PRIVATE lob matching benchmark::benchmark)
  target_compile_features(lob_bench PRIVATE cxx_std_20)
  lob_apply_sanitizers(lob_bench)
endif()

message(STATUS "Project lob configured (C++20). Build type: ${CMAKE_BUILD_TYPE}")
