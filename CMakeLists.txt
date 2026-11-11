cmake_minimum_required(VERSION 3.28)

# Select one native GPU profile before CUDA compiler detection so CMake never
# injects a generic host-toolchain default or builds a mixed-architecture binary.
if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
  set(CMAKE_CUDA_ARCHITECTURES 120a CACHE STRING "Native CUDA architecture profile")
endif()
if(NOT CMAKE_CUDA_ARCHITECTURES MATCHES "^(120a|89|86)$")
  message(FATAL_ERROR
    "NInfer requires exactly one CMAKE_CUDA_ARCHITECTURES profile: 120a, 89 or 86; got "
    "'${CMAKE_CUDA_ARCHITECTURES}'")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
project(ninfer LANGUAGES C CXX CUDA)

# --- Global settings ---------------------------------------------------------
option(NINFER_BUILD_APPS "Build the NInfer CLI, server and perplexity evaluator" ON)
option(BUILD_TESTING "Build NInfer tests" OFF)
option(NINFER_BUILD_BENCHMARKS "Build NInfer benchmarks" OFF)
# Opt-in prefill: eligible Q4/Q5 weights use the Group64 A8 activation
# policy. An explicit compile choice, never selected by the build-profile string.
option(NINFER_NATIVE_PREFILL_GROUP64_A8 "Group64 A8 activation policy for native Q4/Q5 prefill" OFF)
set(NINFER_GROUP64_A8_MIN_TOKENS 33 CACHE STRING "Minimum pass width for Group64 A8 prefill")
if(NINFER_NATIVE_PREFILL_GROUP64_A8)
  if(NOT NINFER_GROUP64_A8_MIN_TOKENS MATCHES "^[1-9][0-9]*$" OR
     NINFER_GROUP64_A8_MIN_TOKENS LESS 33 OR
     NINFER_GROUP64_A8_MIN_TOKENS GREATER 2147483647)
    message(FATAL_ERROR "NINFER_GROUP64_A8_MIN_TOKENS must be an integer in [33, 2147483647]")
  endif()
  if(NOT NINFER_BUILD_PROFILE MATCHES "(^|-)a8(-|$)")
    message(FATAL_ERROR
      "Group64 A8 requires a hyphen-delimited 'a8' component in NINFER_BUILD_PROFILE")
  endif()
elseif(NINFER_BUILD_PROFILE MATCHES "(^|-)a8(-|$)")
  message(FATAL_ERROR "NINFER_BUILD_PROFILE contains 'a8' but Group64 A8 is OFF")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 20)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)        # clangd / IDE tooling
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(CMAKE_GENERATOR MATCHES "Ninja")
  set_property(GLOBAL PROPERTY JOB_POOLS ninfer_link=1)
  set(CMAKE_JOB_POOL_LINK ninfer_link)
endif()

# Tests also exercise product input, logging and protocol adapters.
set(NINFER_BUILD_PRODUCT_SUPPORT OFF)
if(NINFER_BUILD_APPS OR BUILD_TESTING)
  set(NINFER_BUILD_PRODUCT_SUPPORT ON)
endif()

include(cmake/Dependencies.cmake)
include(cmake/BuildIdentity.cmake)
include(cmake/CudaDeterminism.cmake)
include(cmake/NinferTargets.cmake)

# --- Subdirectories ----------------------------------------------------------
add_subdirectory(src)
if(NINFER_BUILD_APPS)
  add_subdirectory(apps)
endif()
if(BUILD_TESTING)
  enable_testing()
  add_subdirectory(tests)
endif()
if(NINFER_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()
