ninfer_add_test(ninfer_scheduler_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../runtime/test_scheduler.cpp"
  LIBRARIES ninfer_runtime_support)

ninfer_add_test(ninfer_speculative_metrics_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_speculative_metrics.cpp")
set_tests_properties(ninfer_speculative_metrics_test PROPERTIES LABELS "host")

add_test(NAME ninfer_engine_cache_gate_test
  COMMAND ${Python3_EXECUTABLE} -B "${CMAKE_CURRENT_LIST_DIR}/../test_engine_cache_gate.py")

ninfer_add_test(ninfer_engine_options_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../runtime/test_engine_options.cpp"
  LIBRARIES ninfer_engine ninfer_core ninfer::json)
set_tests_properties(ninfer_engine_options_test PROPERTIES LABELS "host")
ninfer_add_test(ninfer_engine_checkpoint_control_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_engine_checkpoint_control.cpp"
  LIBRARIES ninfer_core Threads::Threads)
set_tests_properties(ninfer_engine_checkpoint_control_test PROPERTIES LABELS "host")
ninfer_add_test(ninfer_engine_checkpoint_adapter_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_engine_checkpoint_adapter.cpp"
  LIBRARIES ninfer_core Threads::Threads)
set_tests_properties(ninfer_engine_checkpoint_adapter_test PROPERTIES LABELS "host")
ninfer_add_test(ninfer_engine_core_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../runtime/test_engine_core.cpp"
  LIBRARIES ninfer_runtime_support ninfer_core Threads::Threads)
# DeviceContext only binds the Engine worker; the test presents one device to the linked CUDA
# runtime so a GPU-less host runs the production EngineCore without CUDA work.
ninfer_cuda_test_interposition(ninfer_engine_core_test ninfer_runtime_support)
if(NOT WIN32)
  foreach(symbol IN ITEMS cudaGetDeviceCount cudaSetDevice cudaSetDeviceFlags
      cudaGetDeviceProperties cudaGetDeviceProperties_v2 cudaStreamCreateWithFlags
      cudaStreamDestroy)
    target_link_options(ninfer_engine_core_test PRIVATE "LINKER:--wrap=${symbol}")
  endforeach()
endif()
set_tests_properties(ninfer_engine_core_test PROPERTIES LABELS "host" TIMEOUT 120)
add_executable(ninfer_durable_contract_headers_test
  "${CMAKE_CURRENT_LIST_DIR}/../test_durable_contract_headers.cpp")
ninfer_test_includes(ninfer_durable_contract_headers_test)
target_link_libraries(ninfer_durable_contract_headers_test PRIVATE ninfer::json)
add_test(NAME ninfer_durable_contract_headers_test
  COMMAND ninfer_durable_contract_headers_test ${PROJECT_SOURCE_DIR}/tests/fixtures/durable)
set_tests_properties(ninfer_durable_contract_headers_test PROPERTIES LABELS "host")

add_test(NAME ninfer_durable_contracts_test
  COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/test_durable_contracts.py)
set_tests_properties(ninfer_durable_contracts_test PROPERTIES LABELS "host")

ninfer_add_test(ninfer_context_cost_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../runtime/test_context_cost.cpp"
  LIBRARIES ninfer_runtime_support ninfer::json)

ninfer_add_test(ninfer_resource_manager_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../runtime/test_resource_manager.cpp"
  LIBRARIES ninfer_runtime_support)

ninfer_add_test(ninfer_checkpoint_owner_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../runtime/test_checkpoint_owner.cpp"
  LIBRARIES ninfer_runtime_support)
set_tests_properties(ninfer_checkpoint_owner_test PROPERTIES LABELS "host" TIMEOUT 30)

ninfer_add_test(ninfer_export_state_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../runtime/test_export_state.cpp"
          "${PROJECT_SOURCE_DIR}/src/models/common/context/state_store.cpp"
  LIBRARIES ninfer_core)
ninfer_cuda_test_interposition(ninfer_export_state_test)
if(WIN32)
  set_tests_properties(ninfer_export_state_test PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:ninfer_test_cudart>")
else()
  foreach(symbol IN ITEMS cudaGetDeviceCount cudaGetDevice cudaSetDevice cudaSetDeviceFlags
      cudaGetDeviceProperties cudaGetDeviceProperties_v2 cudaStreamCreateWithFlags
      cudaStreamDestroy cudaMallocHost cudaFreeHost cudaMemcpyAsync cudaStreamSynchronize
      cudaGetErrorString)
    target_link_options(ninfer_export_state_test PRIVATE "LINKER:--wrap=${symbol}")
  endforeach()
endif()
set_tests_properties(ninfer_export_state_test PROPERTIES LABELS "host" TIMEOUT 30)

ninfer_add_test(ninfer_kv_capacity_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../runtime/test_kv_capacity.cpp"
  LIBRARIES ninfer_runtime_support)

ninfer_add_test(ninfer_sampling_defaults_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../runtime/test_sampling_defaults.cpp"
  LIBRARIES ninfer_engine ninfer_core)

ninfer_add_test(ninfer_grammar_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_grammar.cpp"
  LIBRARIES ninfer_grammar)

ninfer_add_test(ninfer_regex_choice_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../text/test_regex_choice.cpp"
  LIBRARIES ninfer_grammar ninfer::json)

add_test(NAME ninfer_regex_choice_oracle_test
  COMMAND ${CMAKE_COMMAND} -E env
    "NINFER_REGEX_PROBE=$<TARGET_FILE:ninfer_regex_choice_test>"
    ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/text/test_regex_choice.py)

ninfer_add_test(ninfer_json_schema_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../text/test_json_schema.cpp"
  LIBRARIES ninfer_grammar ninfer::json)

add_test(NAME ninfer_json_schema_oracle_test
  COMMAND ${CMAKE_COMMAND} -E env
    "NINFER_SCHEMA_PROBE=$<TARGET_FILE:ninfer_json_schema_test>"
    ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/text/test_json_schema.py)
