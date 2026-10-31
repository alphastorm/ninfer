# This target deliberately receives no src/, CUDA, artifact, kernel, or target
# include root. It proves that the public product headers stand alone.
add_executable(ninfer_public_api_test "${CMAKE_CURRENT_LIST_DIR}/../test_public_api.cpp")
target_include_directories(ninfer_public_api_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_test(NAME ninfer_public_api_test COMMAND ninfer_public_api_test)

ninfer_add_test(ninfer_rk4v4_codec_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_rk4v4_codec.cpp"
  LIBRARIES ninfer_core)
set_tests_properties(ninfer_rk4v4_codec_test PROPERTIES LABELS "host")

ninfer_add_test(ninfer_checkpoint_io_contract_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_checkpoint_io_contract.cpp"
  LIBRARIES ninfer_checkpoint_contract)

ninfer_add_test(ninfer_direct_storage_checkpoint_backend_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_direct_storage_checkpoint_backend.cpp"
          ${PROJECT_SOURCE_DIR}/src/runtime/windows/direct_storage_checkpoint_backend.cpp
  LIBRARIES ninfer_checkpoint_contract)

set_tests_properties(ninfer_checkpoint_io_contract_test
  ninfer_direct_storage_checkpoint_backend_test PROPERTIES LABELS "host")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  ninfer_add_test(ninfer_io_uring_checkpoint_backend_test
    SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_io_uring_checkpoint_backend.cpp"
            ${PROJECT_SOURCE_DIR}/src/runtime/platform/linux/io_uring_checkpoint_backend.cpp
    LIBRARIES ninfer_checkpoint_contract ninfer_durable_io)
  target_compile_definitions(ninfer_io_uring_checkpoint_backend_test
    PRIVATE NINFER_IO_URING_TESTING=1)
  set_tests_properties(ninfer_io_uring_checkpoint_backend_test
    PROPERTIES LABELS "host" SKIP_RETURN_CODE 77)
  ninfer_add_test(ninfer_io_uring_checkpoint_read_queue_test
    SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_io_uring_checkpoint_read_queue.cpp"
            ${PROJECT_SOURCE_DIR}/src/runtime/platform/linux/io_uring_checkpoint_backend.cpp
    LIBRARIES ninfer_checkpoint_contract ninfer_durable_io)
  target_compile_definitions(ninfer_io_uring_checkpoint_read_queue_test
    PRIVATE NINFER_IO_URING_TESTING=1)
  set_tests_properties(ninfer_io_uring_checkpoint_read_queue_test
    PROPERTIES LABELS "host" SKIP_RETURN_CODE 77 TIMEOUT 60)
  foreach(case IN ITEMS handle-allocation teardown)
    add_test(NAME ninfer_io_uring_checkpoint_${case}_test
      COMMAND ninfer_io_uring_checkpoint_read_queue_test ${case})
    set_tests_properties(ninfer_io_uring_checkpoint_${case}_test
      PROPERTIES LABELS "host" TIMEOUT 60)
  endforeach()
endif()
ninfer_add_test(ninfer_uint128_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../cpu/test_uint128.cpp")
add_test(NAME ninfer_uint128_exact_test
  COMMAND ${Python3_EXECUTABLE} -B "${CMAKE_CURRENT_LIST_DIR}/../cpu/test_uint128_exact.py"
          $<TARGET_FILE:ninfer_uint128_test>)
set_tests_properties(ninfer_uint128_test ninfer_uint128_exact_test PROPERTIES LABELS "host")

ninfer_add_test(ninfer_durable_file_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../io/test_durable_file.cpp"
  LIBRARIES ninfer_durable_io)
set_tests_properties(ninfer_durable_file_test PROPERTIES LABELS "host")
if(WIN32)
  foreach(case IN ITEMS pinned-destination reparse-guard identity-guards directory-publication extended-paths)
    add_test(NAME ninfer_durable_file_${case}_test COMMAND ninfer_durable_file_test ${case})
    set_tests_properties(ninfer_durable_file_${case}_test PROPERTIES LABELS "host;durable")
  endforeach()
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(ninfer_durable_file_test PRIVATE "LINKER:--wrap=fsync")
endif()

ninfer_add_test(ninfer_file_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../io/test_file.cpp"
  LIBRARIES ninfer_core)
set_tests_properties(ninfer_file_test PROPERTIES LABELS "host" TIMEOUT 30)

if(WIN32)
  ninfer_add_test(ninfer_direct_storage_checkpoint_windows_test
    SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_direct_storage_checkpoint_windows.cpp"
    LIBRARIES ninfer_checkpoint_directstorage_windows)
  add_custom_command(TARGET ninfer_direct_storage_checkpoint_windows_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${NINFER_DIRECTSTORAGE_DLLS}
            $<TARGET_FILE_DIR:ninfer_direct_storage_checkpoint_windows_test>
    VERBATIM)
  set_tests_properties(ninfer_direct_storage_checkpoint_windows_test PROPERTIES
    LABELS "windows;directstorage" SKIP_RETURN_CODE 77 TIMEOUT 180)
endif()
foreach(part IN ITEMS system task_pool executor)
  ninfer_add_test(ninfer_cpu_${part}_test
    SOURCES "${CMAKE_CURRENT_LIST_DIR}/../cpu/test_${part}.cpp"
    LIBRARIES ninfer_cpu)
  set_tests_properties(ninfer_cpu_${part}_test PROPERTIES TIMEOUT 30)
endforeach()

ninfer_add_test(ninfer_device_test       SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_device.cpp"
  LIBRARIES ninfer_core)

set_tests_properties(ninfer_device_test PROPERTIES
  ENVIRONMENT_MODIFICATION "NINFER_CUDA_SYNC=unset:")
set(sync_modes spin blocking yield auto)
set(sync_flags 1 4 2 0)
foreach(mode flags IN ZIP_LISTS sync_modes sync_flags)
  add_test(NAME ninfer_device_sync_${mode}_test COMMAND ninfer_device_test ${flags})
  set_tests_properties(ninfer_device_sync_${mode}_test PROPERTIES
    ENVIRONMENT "NINFER_CUDA_SYNC=${mode}" SKIP_RETURN_CODE 77)
endforeach()
foreach(mode IN ITEMS invalid empty)
  add_test(NAME ninfer_device_sync_${mode}_test COMMAND ninfer_device_test --invalid-sync)
endforeach()
set_tests_properties(ninfer_device_sync_invalid_test PROPERTIES
  ENVIRONMENT "NINFER_CUDA_SYNC=invalid")
set_tests_properties(ninfer_device_sync_empty_test PROPERTIES
  ENVIRONMENT "NINFER_CUDA_SYNC=")

ninfer_add_test(ninfer_decode_graph_test SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_decode_graph.cpp"
  LIBRARIES ninfer_core)

ninfer_add_test(ninfer_gpu_keep_warm_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_gpu_keep_warm.cpp"
  LIBRARIES ninfer_core)
set_tests_properties(ninfer_gpu_keep_warm_test PROPERTIES LABELS "gpu;cuda_core" SKIP_RETURN_CODE 77)

ninfer_add_test(ninfer_tensor_test       SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_tensor.cpp"
  LIBRARIES ninfer_core)

ninfer_add_test(ninfer_memory_test SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_memory.cpp"
  LIBRARIES ninfer_core)

ninfer_add_test(ninfer_nf4_test SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_nf4.cpp"
  LIBRARIES ninfer_core)
add_test(NAME ninfer_nf4_exact_test
  COMMAND ${Python3_EXECUTABLE} -B "${CMAKE_CURRENT_LIST_DIR}/../test_nf4_exact.py"
    $<TARGET_FILE:ninfer_nf4_test>)

ninfer_add_test(ninfer_read_queue_test SOURCES "${CMAKE_CURRENT_LIST_DIR}/../io/test_read_queue.cpp"
  LIBRARIES ninfer_core ${NINFER_URING_TARGET})
if(NOT WIN32)
  target_link_options(ninfer_read_queue_test PRIVATE "LINKER:--wrap=io_uring_submit")
endif()
set_tests_properties(ninfer_read_queue_test PROPERTIES TIMEOUT 30)

ninfer_add_test(ninfer_arena_test        SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_arena.cpp"
  LIBRARIES ninfer_core)

ninfer_add_test(ninfer_kv_cache_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_kv_cache.cpp"
  LIBRARIES ninfer_core)

ninfer_add_test(ninfer_host_context_arena_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_host_context_arena.cpp"
  LIBRARIES ninfer_core)

ninfer_add_test(ninfer_state_store_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_state_store.cpp"
  LIBRARIES ninfer_core)

ninfer_add_test(ninfer_gdn_replay_records_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_gdn_replay_records.cpp"
  LIBRARIES ninfer_core)

set_tests_properties(
  ninfer_device_test
  ninfer_decode_graph_test
  ninfer_arena_test
  ninfer_kv_cache_test
  ninfer_host_context_arena_test
  ninfer_state_store_test
  PROPERTIES SKIP_RETURN_CODE 77)

ninfer_add_test(ninfer_host_timing_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_host_timing.cpp"
  LIBRARIES ninfer_core)

ninfer_add_test(ninfer_jinja_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../text/test_jinja.cpp"
  LIBRARIES ninfer_jinja ninfer::json)

add_test(NAME ninfer_chat_templates_test
  COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/text/test_chat_templates.py
          $<TARGET_FILE:ninfer_jinja_test>)

ninfer_add_test(ninfer_weight_bank_test SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_weight_bank.cpp"
  LIBRARIES ninfer_core)

ninfer_add_test(ninfer_expert_cache_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_expert_cache.cu"
          "${CMAKE_CURRENT_LIST_DIR}/../expert_cache_cuda_errors.cpp"
  LIBRARIES ninfer_core)
ninfer_cuda_test_interposition(ninfer_expert_cache_test)
set_tests_properties(ninfer_expert_cache_test PROPERTIES TIMEOUT 60)

if(NOT WIN32)
foreach(symbol IN ITEMS
    cudaEventRecord cudaMemcpyAsync cudaMemcpyBatchAsync cudaStreamSynchronize cudaStreamWaitEvent
    cudaFree cudaFreeHost)
  target_link_options(ninfer_expert_cache_test PRIVATE "LINKER:--wrap=${symbol}")
endforeach()
endif()
