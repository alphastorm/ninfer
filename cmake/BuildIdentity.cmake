# Include after Dependencies.cmake has resolved the CUDA toolkit and build type.
include_guard(GLOBAL)

set(NINFER_BUILD_PROFILE "generic" CACHE STRING "Deployment build profile identity")
set(NINFER_UPSTREAM_BASE_SHA "unknown" CACHE STRING "Exact upstream base commit")
set(NINFER_PATCH_STACK_SHA "auto" CACHE STRING "Exact patch-stack commit or auto")
set(NINFER_SOURCE_DIRTY "auto" CACHE STRING "Source-dirty identity: auto or ON")
set_property(CACHE NINFER_SOURCE_DIRTY PROPERTY STRINGS auto ON)

if(NOT NINFER_BUILD_PROFILE MATCHES "^[A-Za-z0-9._-]+$")
  message(FATAL_ERROR "NINFER_BUILD_PROFILE must contain only release-safe characters")
endif()
string(LENGTH "${NINFER_BUILD_PROFILE}" profile_length)
if(profile_length GREATER 64)
  message(FATAL_ERROR "NINFER_BUILD_PROFILE must contain at most 64 characters")
endif()
if(NOT NINFER_SOURCE_DIRTY STREQUAL "auto" AND NOT NINFER_SOURCE_DIRTY STREQUAL "ON")
  message(FATAL_ERROR "NINFER_SOURCE_DIRTY must be auto or ON; clean state is verified")
endif()

set(NINFER_PATCH_STACK_SHA_RESOLVED "${NINFER_PATCH_STACK_SHA}")
# Missing Git metadata is not evidence of a clean source archive. The lifecycle
# builder materializes a detached Git tree, so it uses this same verification path.
set(NINFER_SOURCE_DIRTY_BOOL 1)
find_package(Git QUIET)
if(Git_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
  if(NINFER_PATCH_STACK_SHA STREQUAL "auto")
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -C "${PROJECT_SOURCE_DIR}" rev-parse HEAD
      OUTPUT_VARIABLE NINFER_PATCH_STACK_SHA_RESOLVED
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE git_head_result)
    if(NOT git_head_result EQUAL 0)
      set(NINFER_PATCH_STACK_SHA_RESOLVED "unknown")
    endif()
  endif()
  if(NINFER_SOURCE_DIRTY STREQUAL "auto")
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -C "${PROJECT_SOURCE_DIR}"
              status --porcelain --untracked-files=all
      OUTPUT_VARIABLE git_status
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE git_status_result)
    if(git_status_result EQUAL 0 AND git_status STREQUAL "")
      set(NINFER_SOURCE_DIRTY_BOOL 0)
    endif()
  endif()
elseif(NINFER_PATCH_STACK_SHA STREQUAL "auto")
  set(NINFER_PATCH_STACK_SHA_RESOLVED "unknown")
endif()

foreach(sha IN ITEMS NINFER_UPSTREAM_BASE_SHA NINFER_PATCH_STACK_SHA_RESOLVED)
  if(NOT "${${sha}}" STREQUAL "unknown")
    string(LENGTH "${${sha}}" sha_length)
    if(NOT "${${sha}}" MATCHES "^[0-9a-f]+$" OR NOT sha_length EQUAL 40)
      message(FATAL_ERROR "${sha} must be a full lowercase Git SHA or unknown")
    endif()
  endif()
endforeach()

file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/generated")
configure_file(
  "${PROJECT_SOURCE_DIR}/cmake/ninfer_build_info_config.h.in"
  "${PROJECT_BINARY_DIR}/generated/ninfer_build_info_config.h"
  @ONLY)

# Compile owners must depend on this target: stale configure-time provenance must
# never survive an incremental build from a different commit or clean/dirty state.
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
