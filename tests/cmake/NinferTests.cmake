# Test declarations are included from tests/CMakeLists.txt so executable paths and
# CTest working directories stay under build/tests.
function(ninfer_test_includes target)
  ninfer_internal_includes(${target})
  target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/tests)
endfunction()

function(ninfer_add_test name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "NEEDS_SOURCE_DIR" "" "SOURCES;LIBRARIES")
  add_executable(${name} ${arg_SOURCES})
  ninfer_test_includes(${name})
  target_link_libraries(${name} PRIVATE ${arg_LIBRARIES})
  if(arg_NEEDS_SOURCE_DIR)
    target_compile_definitions(${name} PRIVATE
      NINFER_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
      NINFER_PYTHON_EXECUTABLE="${Python3_EXECUTABLE}")
  endif()
  add_test(NAME ${name} COMMAND ${name})
endfunction()

# CUDA 13.3's Windows import archive defines every API in one loader object. Pulling
# any unwrapped API also pulls definitions that collide with these tests' wrappers.
# A conventional per-export import archive leaves each wrapped symbol defined once
# by the test, and all other calls still enter the same real cudart DLL.
function(ninfer_cuda_test_interposition target)
  if(NOT WIN32)
    return()
  endif()
  if(NOT TARGET ninfer_test_cudart)
    find_file(ninfer_test_cudart_dll NAMES cudart64_13.dll
      PATHS "${CUDAToolkit_BIN_DIR}" "${CUDAToolkit_BIN_DIR}/x64"
      NO_DEFAULT_PATH REQUIRED)
    execute_process(
      COMMAND "${CMAKE_LINKER}" /dump /exports "${ninfer_test_cudart_dll}"
      RESULT_VARIABLE status OUTPUT_VARIABLE exports ERROR_VARIABLE error)
    if(NOT status EQUAL 0)
      message(FATAL_ERROR "Cannot inspect the test CUDA runtime: ${error}")
    endif()
    string(REGEX MATCHALL
      "\n[ \t]+[0-9]+[ \t]+[0-9A-Fa-f]+[ \t]+[0-9A-Fa-f]+[ \t]+[^ \t\r\n]+"
      export_rows "${exports}")
    if(NOT export_rows)
      message(FATAL_ERROR "The test CUDA runtime has no named exports")
    endif()
    get_filename_component(dll_name "${ninfer_test_cudart_dll}" NAME)
    set(definition "LIBRARY ${dll_name}\nEXPORTS\n")
    foreach(row IN LISTS export_rows)
      string(REGEX REPLACE
        "^\n[ \t]+[0-9]+[ \t]+[0-9A-Fa-f]+[ \t]+[0-9A-Fa-f]+[ \t]+"
        "" symbol "${row}")
      string(APPEND definition "${symbol}\n")
    endforeach()
    set(def_file "${CMAKE_CURRENT_BINARY_DIR}/cudart-test.def")
    set(import_file "${CMAKE_CURRENT_BINARY_DIR}/cudart-test.lib")
    file(GENERATE OUTPUT "${def_file}" CONTENT "${definition}")
    add_custom_command(OUTPUT "${import_file}"
      COMMAND "${CMAKE_AR}" /nologo /machine:X64 "/def:${def_file}" "/out:${import_file}"
      DEPENDS "${def_file}" "${ninfer_test_cudart_dll}" VERBATIM)
    add_custom_target(ninfer_test_cudart_archive DEPENDS "${import_file}")
    add_library(ninfer_test_cudart SHARED IMPORTED GLOBAL)
    set_target_properties(ninfer_test_cudart PROPERTIES
      IMPORTED_LOCATION "${ninfer_test_cudart_dll}" IMPORTED_IMPLIB "${import_file}")
    add_dependencies(ninfer_test_cudart ninfer_test_cudart_archive)
  endif()
  # Resolve references from CUDA-bearing static archives before considering the SDK's
  # monolithic loader. ARGN lists additional existing dependencies of the test.
  target_link_libraries(${target} PRIVATE ${ARGN} ninfer_core ninfer_test_cudart)
endfunction()

# Apply these to the translation unit containing the oracle, including shared
# test support libraries. Executable options do not propagate into those libraries.
function(ninfer_op_oracle_options target)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE
      $<$<COMPILE_LANGUAGE:CXX>:-fno-fast-math>
      $<$<COMPILE_LANGUAGE:CXX>:-ffp-contract=off>)
  endif()
endfunction()

function(ninfer_add_op_test name)
  ninfer_add_test(${name} ${ARGN})
  set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
  set_property(TEST ${name} APPEND PROPERTY LABELS ops)
  ninfer_op_oracle_options(${name})
endfunction()
