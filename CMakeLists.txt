cmake_minimum_required(VERSION 3.28)

# The current implementation is compiled only for sm_120a. Set and validate the
# architecture before CUDA compiler detection so CMake never injects a generic
# host-toolchain default or accepts an unsupported target.
if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
  set(CMAKE_CUDA_ARCHITECTURES 120a CACHE STRING "CUDA architectures to build")
endif()
if(NOT CMAKE_CUDA_ARCHITECTURES STREQUAL "120a")
  message(FATAL_ERROR
    "NInfer supports only CMAKE_CUDA_ARCHITECTURES=120a; got "
    "'${CMAKE_CUDA_ARCHITECTURES}'")
endif()

project(ninfer LANGUAGES C CXX CUDA)

# --- Global settings ---------------------------------------------------------
option(NINFER_BUILD_APPS "Build the NInfer CLI and server" ON)
option(BUILD_TESTING "Build NInfer tests" OFF)
option(NINFER_BUILD_BENCHMARKS "Build NInfer benchmarks" OFF)
set(NINFER_BUILD_PROFILE "generic" CACHE STRING "Deployment build profile identity")
set(NINFER_UPSTREAM_BASE_SHA "unknown" CACHE STRING "Exact upstream base commit")
set(NINFER_PATCH_STACK_SHA "auto" CACHE STRING "Exact patch-stack commit or auto")

set(NINFER_PATCH_STACK_SHA_RESOLVED "${NINFER_PATCH_STACK_SHA}")
set(NINFER_SOURCE_DIRTY_BOOL 0)
find_package(Git QUIET)
if(Git_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
  if(NINFER_PATCH_STACK_SHA STREQUAL "auto")
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -C "${PROJECT_SOURCE_DIR}" rev-parse HEAD
      OUTPUT_VARIABLE NINFER_PATCH_STACK_SHA_RESOLVED
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE NINFER_GIT_REV_PARSE_RESULT)
    if(NOT NINFER_GIT_REV_PARSE_RESULT EQUAL 0)
      set(NINFER_PATCH_STACK_SHA_RESOLVED "unknown")
    endif()
  endif()
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${PROJECT_SOURCE_DIR}" status --porcelain
    OUTPUT_VARIABLE NINFER_GIT_STATUS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE NINFER_GIT_STATUS_RESULT)
  if(NOT NINFER_GIT_STATUS_RESULT EQUAL 0 OR NOT NINFER_GIT_STATUS STREQUAL "")
    set(NINFER_SOURCE_DIRTY_BOOL 1)
  endif()
elseif(NINFER_PATCH_STACK_SHA STREQUAL "auto")
  set(NINFER_PATCH_STACK_SHA_RESOLVED "unknown")
endif()

if(NOT NINFER_UPSTREAM_BASE_SHA STREQUAL "unknown" AND
   NOT NINFER_UPSTREAM_BASE_SHA MATCHES "^[0-9a-f]{40}$")
  message(FATAL_ERROR "NINFER_UPSTREAM_BASE_SHA must be a full lowercase Git SHA")
endif()
if(NOT NINFER_PATCH_STACK_SHA_RESOLVED STREQUAL "unknown" AND
   NOT NINFER_PATCH_STACK_SHA_RESOLVED MATCHES "^[0-9a-f]{40}$")
  message(FATAL_ERROR "NINFER_PATCH_STACK_SHA must be auto or a full lowercase Git SHA")
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
  set_property(GLOBAL PROPERTY JOB_POOLS cuda_link=1)
  set(CMAKE_JOB_POOL_LINK cuda_link)
endif()

if(CMAKE_CUDA_COMPILER_VERSION VERSION_LESS 13.1)
  message(FATAL_ERROR
    "NInfer requires CUDA 13.1 or newer; found CUDA compiler "
    "${CMAKE_CUDA_COMPILER_VERSION}")
endif()

set(NINFER_BUILD_MEDIA_ACQUIRE OFF)
if(NINFER_BUILD_APPS OR BUILD_TESTING)
  set(NINFER_BUILD_MEDIA_ACQUIRE ON)
endif()

set(NINFER_BUILD_PROMPT_INPUT OFF)
if(NINFER_BUILD_APPS)
  set(NINFER_BUILD_PROMPT_INPUT ON)
endif()

set(NINFER_BUILD_SERVE OFF)
if(NINFER_BUILD_APPS OR BUILD_TESTING)
  set(NINFER_BUILD_SERVE ON)
endif()

find_package(CUDAToolkit REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
  libavformat>=60 libavcodec>=60 libavutil>=58 libswscale>=7)
if(NINFER_BUILD_MEDIA_ACQUIRE)
  pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl>=7.85)
endif()
find_package(Threads REQUIRED)

file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/generated")
configure_file(
  "${PROJECT_SOURCE_DIR}/cmake/ninfer_build_info_config.h.in"
  "${PROJECT_BINARY_DIR}/generated/ninfer_build_info_config.h"
  @ONLY)

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
