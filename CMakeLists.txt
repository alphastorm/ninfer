cmake_minimum_required(VERSION 3.28)

# The current implementation supports Ampere sm_86 and early Ada sm_89. Set and validate the
# architecture before CUDA compiler detection so CMake never injects a generic
# host-toolchain default or accepts an unsupported target.
if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
  set(CMAKE_CUDA_ARCHITECTURES 86 CACHE STRING "CUDA architectures to build")
endif()
if(NOT CMAKE_CUDA_ARCHITECTURES MATCHES "^(86|89)$")
  message(FATAL_ERROR
    "NInfer supports CMAKE_CUDA_ARCHITECTURES=86 or 89; got "
    "'${CMAKE_CUDA_ARCHITECTURES}'")
endif()

project(ninfer LANGUAGES C CXX CUDA)

# --- Global settings ---------------------------------------------------------
option(NINFER_BUILD_APPS "Build the NInfer CLI and server" ON)
option(BUILD_TESTING "Build NInfer tests" OFF)
option(NINFER_BUILD_BENCHMARKS "Build NInfer benchmarks" OFF)
set(NINFER_BUILD_PROFILE "generic" CACHE STRING "Deployment build profile identity")
set(NINFER_UPSTREAM_BASE_SHA "ef6ecc3c139b43fc4d3e1b92df474305e8429544" CACHE STRING
  "Exact upstream base commit")
set(NINFER_PATCH_STACK_SHA "auto" CACHE STRING "Exact patch-stack commit or auto")
set(NINFER_SOURCE_DIRTY "auto" CACHE STRING "Source-dirty identity: auto or ON")
set_property(CACHE NINFER_SOURCE_DIRTY PROPERTY STRINGS auto ON)

string(LENGTH "${NINFER_BUILD_PROFILE}" NINFER_BUILD_PROFILE_LENGTH)
if(NOT NINFER_BUILD_PROFILE MATCHES "^[A-Za-z0-9][A-Za-z0-9._-]*$" OR
   NINFER_BUILD_PROFILE_LENGTH GREATER 64)
  message(FATAL_ERROR
    "NINFER_BUILD_PROFILE must contain 1..64 release-safe name characters")
endif()

set(NINFER_PATCH_STACK_SHA_RESOLVED "${NINFER_PATCH_STACK_SHA}")
# Unknown source state is conservatively dirty. Only a successful Git status or a release-tool
# attestation for an exact archive may identify a build as clean.
set(NINFER_SOURCE_DIRTY_BOOL 1)
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
  if(NINFER_SOURCE_DIRTY STREQUAL "auto")
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -C "${PROJECT_SOURCE_DIR}"
              status --porcelain --untracked-files=all
      OUTPUT_VARIABLE NINFER_GIT_STATUS
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE NINFER_GIT_STATUS_RESULT)
    if(NINFER_GIT_STATUS_RESULT EQUAL 0 AND NINFER_GIT_STATUS STREQUAL "")
      set(NINFER_SOURCE_DIRTY_BOOL 0)
    endif()
  endif()
else()
  if(NINFER_PATCH_STACK_SHA STREQUAL "auto")
    set(NINFER_PATCH_STACK_SHA_RESOLVED "unknown")
  endif()
endif()

if(NINFER_SOURCE_DIRTY STREQUAL "ON")
  set(NINFER_SOURCE_DIRTY_BOOL 1)
elseif(NOT NINFER_SOURCE_DIRTY STREQUAL "auto")
  message(FATAL_ERROR "NINFER_SOURCE_DIRTY must be auto or ON; clean state is verified")
endif()

if(NOT NINFER_UPSTREAM_BASE_SHA STREQUAL "unknown")
  string(LENGTH "${NINFER_UPSTREAM_BASE_SHA}" NINFER_UPSTREAM_BASE_SHA_LENGTH)
  if(NOT NINFER_UPSTREAM_BASE_SHA MATCHES "^[0-9a-f]+$" OR
     NOT NINFER_UPSTREAM_BASE_SHA_LENGTH EQUAL 40)
    message(FATAL_ERROR "NINFER_UPSTREAM_BASE_SHA must be a full lowercase Git SHA")
  endif()
endif()
if(NOT NINFER_PATCH_STACK_SHA_RESOLVED STREQUAL "unknown")
  string(LENGTH "${NINFER_PATCH_STACK_SHA_RESOLVED}" NINFER_PATCH_STACK_SHA_LENGTH)
  if(NOT NINFER_PATCH_STACK_SHA_RESOLVED MATCHES "^[0-9a-f]+$" OR
     NOT NINFER_PATCH_STACK_SHA_LENGTH EQUAL 40)
    message(FATAL_ERROR "NINFER_PATCH_STACK_SHA must be auto or a full lowercase Git SHA")
  endif()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 20)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)        # clangd / IDE tooling
if(MSVC)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
  add_compile_options(
    $<$<COMPILE_LANGUAGE:C,CXX>:/Zc:preprocessor>
    $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/Zc:preprocessor>)
  # CUDA's static runtime requests LIBCMT even when the application and vcpkg dependencies use
  # the project-wide /MD runtime. Keep one CRT in the final process.
  add_link_options(/NODEFAULTLIB:LIBCMT)
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(CMAKE_GENERATOR MATCHES "Ninja")
  set_property(GLOBAL PROPERTY JOB_POOLS cuda_link=1)
  set(CMAKE_JOB_POOL_LINK cuda_link)
endif()

if(CMAKE_CUDA_COMPILER_VERSION VERSION_LESS 12.8)
  message(FATAL_ERROR
    "NInfer requires CUDA 12.8 or newer; found CUDA compiler "
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
if(WIN32)
  set(NINFER_CUDART_TARGET CUDA::cudart_static)
else()
  set(NINFER_CUDART_TARGET CUDA::cudart)
endif()
if(WIN32 OR DEFINED VCPKG_TARGET_TRIPLET)
  find_package(FFMPEG REQUIRED)
  add_library(ninfer_ffmpeg_dependencies INTERFACE)
  target_include_directories(ninfer_ffmpeg_dependencies INTERFACE ${FFMPEG_INCLUDE_DIRS})
  target_link_directories(ninfer_ffmpeg_dependencies INTERFACE ${FFMPEG_LIBRARY_DIRS})
  target_link_libraries(ninfer_ffmpeg_dependencies INTERFACE ${FFMPEG_LIBRARIES})
  set(NINFER_FFMPEG_TARGET ninfer_ffmpeg_dependencies)
  if(NINFER_BUILD_MEDIA_ACQUIRE)
    find_package(CURL 7.85 REQUIRED)
    set(NINFER_CURL_TARGET CURL::libcurl)
  endif()
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat>=60 libavcodec>=60 libavutil>=58 libswscale>=7)
  set(NINFER_FFMPEG_TARGET PkgConfig::FFMPEG)
  if(NINFER_BUILD_MEDIA_ACQUIRE)
    pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl>=7.85)
    set(NINFER_CURL_TARGET PkgConfig::LIBCURL)
  endif()
endif()
if(NINFER_BUILD_SERVE)
  find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
  find_package(Threads REQUIRED)
endif()

if(WIN32)
  include(FetchContent)
  FetchContent_Declare(
    directstorage
    URL "https://www.nuget.org/api/v2/package/Microsoft.Direct3D.DirectStorage/1.3.0"
    DOWNLOAD_NAME "directstorage.1.3.0.zip"
  )
  FetchContent_MakeAvailable(directstorage)

  add_library(ninfer_directstorage INTERFACE)
  target_include_directories(ninfer_directstorage INTERFACE
    ${directstorage_SOURCE_DIR}/native/include
  )
  target_link_libraries(ninfer_directstorage INTERFACE
    ${directstorage_SOURCE_DIR}/native/lib/x64/dstorage.lib
    d3d12.lib
    dxgi.lib
  )

  set(DSTORAGE_DLLS
    "${directstorage_SOURCE_DIR}/native/bin/x64/dstorage.dll"
    "${directstorage_SOURCE_DIR}/native/bin/x64/dstoragecore.dll"
  )
endif()

file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/generated")
configure_file(
  "${PROJECT_SOURCE_DIR}/cmake/ninfer_build_info_config.h.in"
  "${PROJECT_BINARY_DIR}/generated/ninfer_build_info_config.h"
  @ONLY)

# Source edits do not necessarily retrigger CMake configuration. Recheck the configured commit and
# dirty state before compiling so a binary can never retain provenance for different source bytes.
add_custom_target(ninfer_verify_build_source ALL
  COMMAND "${CMAKE_COMMAND}"
    "-DNINFER_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-DNINFER_CONFIGURED_PATCH_STACK_SHA=${NINFER_PATCH_STACK_SHA_RESOLVED}"
    "-DNINFER_CONFIGURED_SOURCE_DIRTY=${NINFER_SOURCE_DIRTY_BOOL}"
    "-DNINFER_SOURCE_DIRTY_MODE=${NINFER_SOURCE_DIRTY}"
    "-DNINFER_GIT_EXECUTABLE=${GIT_EXECUTABLE}"
    -P "${PROJECT_SOURCE_DIR}/cmake/verify_build_source.cmake"
  COMMENT "Verifying NInfer build source identity"
  VERBATIM)

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
