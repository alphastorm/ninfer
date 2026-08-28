cmake_minimum_required(VERSION 3.28)

foreach(required_variable IN ITEMS
    NINFER_SOURCE_DIR
    NINFER_CONFIGURED_PATCH_STACK_SHA
    NINFER_CONFIGURED_SOURCE_DIRTY
    NINFER_SOURCE_DIRTY_MODE)
  if(NOT DEFINED ${required_variable})
    message(FATAL_ERROR "${required_variable} is required")
  endif()
endforeach()

if(NOT NINFER_CONFIGURED_SOURCE_DIRTY MATCHES "^[01]$")
  message(FATAL_ERROR "NINFER_CONFIGURED_SOURCE_DIRTY must be 0 or 1")
endif()
if(NOT NINFER_SOURCE_DIRTY_MODE STREQUAL "auto" AND
   NOT NINFER_SOURCE_DIRTY_MODE STREQUAL "ON")
  message(FATAL_ERROR "NINFER_SOURCE_DIRTY_MODE must be auto or ON")
endif()

# A source tree without Git metadata is always configured dirty and cannot enter release packaging.
# A live worktree must still match the identity captured at configure time whenever compilation
# runs, or an incremental build could label different source bytes as clean.
if(NOT EXISTS "${NINFER_SOURCE_DIR}/.git")
  return()
endif()
if(NOT DEFINED NINFER_GIT_EXECUTABLE OR NINFER_GIT_EXECUTABLE STREQUAL "" OR
   NOT EXISTS "${NINFER_GIT_EXECUTABLE}")
  if(NINFER_CONFIGURED_SOURCE_DIRTY STREQUAL "0")
    message(FATAL_ERROR "Git is required to verify a clean configured source identity")
  endif()
  return()
endif()

execute_process(
  COMMAND "${NINFER_GIT_EXECUTABLE}" -C "${NINFER_SOURCE_DIR}" rev-parse HEAD
  OUTPUT_VARIABLE current_patch_stack_sha
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE rev_parse_result)
if(NOT rev_parse_result EQUAL 0)
  message(FATAL_ERROR "failed to read the current source commit")
endif()
if(NOT NINFER_CONFIGURED_PATCH_STACK_SHA STREQUAL "unknown" AND
   NOT current_patch_stack_sha STREQUAL NINFER_CONFIGURED_PATCH_STACK_SHA)
  message(FATAL_ERROR "source commit changed since configure; rerun CMake before building")
endif()

if(NINFER_SOURCE_DIRTY_MODE STREQUAL "auto")
  execute_process(
    COMMAND "${NINFER_GIT_EXECUTABLE}" -C "${NINFER_SOURCE_DIR}"
            status --porcelain --untracked-files=all
    OUTPUT_VARIABLE git_status
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE git_status_result)
  if(NOT git_status_result EQUAL 0)
    message(FATAL_ERROR "failed to read the current source status")
  endif()

  set(current_source_dirty 0)
  if(NOT git_status STREQUAL "")
    set(current_source_dirty 1)
  endif()
  if(NOT current_source_dirty STREQUAL NINFER_CONFIGURED_SOURCE_DIRTY)
    message(FATAL_ERROR "source dirty state changed since configure; rerun CMake before building")
  endif()
endif()
