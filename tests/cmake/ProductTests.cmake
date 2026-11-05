ninfer_add_test(ninfer_media_decode_test
  HOST_LANES bounded native
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_media_decode.cpp"
  LIBRARIES ninfer_media_decode)

ninfer_add_test(ninfer_prompt_input_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_prompt_input.cpp"
  LIBRARIES ninfer_product_prompt_input)

ninfer_add_test(ninfer_pretty_logging_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_pretty_logging.cpp"
  LIBRARIES ninfer_product_logging)

ninfer_add_test(ninfer_perplexity_evaluation_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_perplexity_evaluation.cpp"
          ${PROJECT_SOURCE_DIR}/apps/perplexity/evaluation.cpp
  LIBRARIES ninfer_core)

target_include_directories(ninfer_perplexity_evaluation_test PRIVATE
  ${PROJECT_SOURCE_DIR}/apps/perplexity)

ninfer_add_test(ninfer_cli_options_test
  HOST_LANES bounded native windows
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_cli_options.cpp" ${PROJECT_SOURCE_DIR}/apps/cli/options.cpp
  LIBRARIES ninfer_runtime_support ninfer_product_logging)

target_include_directories(ninfer_cli_options_test PRIVATE ${PROJECT_SOURCE_DIR}/apps/cli)

ninfer_add_test(ninfer_openai_schema_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_openai_schema.cpp"
  LIBRARIES ninfer_serve)

ninfer_add_test(ninfer_openai_responses_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_openai_responses.cpp"
  LIBRARIES ninfer_serve)

ninfer_add_test(ninfer_openai_responses_store_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_openai_responses_store.cpp"
  LIBRARIES ninfer_serve_state)
set_tests_properties(ninfer_openai_responses_store_test PROPERTIES LABELS "host")

ninfer_add_test(ninfer_session_checkpoint_service_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_session_checkpoint_service.cpp"
  LIBRARIES ninfer_serve_state)
set_tests_properties(ninfer_session_checkpoint_service_test PROPERTIES LABELS "host" TIMEOUT 30)

ninfer_add_test(ninfer_session_checkpoint_export_test
  HOST_LANES bounded native
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_session_checkpoint_export.cpp"
          "${PROJECT_SOURCE_DIR}/src/serve/session_checkpoint_service.cpp"
          "${PROJECT_SOURCE_DIR}/src/serve/opaque_id.cpp"
  LIBRARIES ninfer_session_checkpoint_store)
set_tests_properties(ninfer_session_checkpoint_export_test PROPERTIES TIMEOUT 30)
if(WIN32)
  target_link_libraries(ninfer_session_checkpoint_export_test PRIVATE bcrypt advapi32)
endif()

add_test(NAME ninfer_serve_restore_runner_test
  COMMAND ${Python3_EXECUTABLE} -B "${CMAKE_CURRENT_LIST_DIR}/../test_serve_restore_runner.py")
set_tests_properties(ninfer_serve_restore_runner_test PROPERTIES LABELS "host")
ninfer_test_host_lanes(ninfer_serve_restore_runner_test ninfer_session_checkpoint_export_test bounded native)

if(TARGET ninfer-serve)
  add_test(NAME ninfer_serve_restore_real_test
    COMMAND ${Python3_EXECUTABLE} -B "${CMAKE_CURRENT_LIST_DIR}/../test_serve_restore_real.py"
            --server $<TARGET_FILE:ninfer-serve>)
  set_tests_properties(ninfer_serve_restore_real_test PROPERTIES
    LABELS "gpu;real;durable" SKIP_RETURN_CODE 77 TIMEOUT 900 RUN_SERIAL TRUE)
  add_test(NAME ninfer_serve_shutdown_capacity_real_test
    COMMAND ${Python3_EXECUTABLE} -B "${CMAKE_CURRENT_LIST_DIR}/../test_serve_shutdown_capacity_real.py"
            --server $<TARGET_FILE:ninfer-serve>)
  set_tests_properties(ninfer_serve_shutdown_capacity_real_test PROPERTIES
    LABELS "gpu;real;durable" SKIP_RETURN_CODE 77 TIMEOUT 1800 RUN_SERIAL TRUE)
endif()

# The close fault uses the Linux link-time stdio wrappers shared with the store tests.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  ninfer_add_test(ninfer_session_checkpoint_service_real_test
    SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_session_checkpoint_service_real.cpp"
    LIBRARIES ninfer_serve)
  target_link_options(ninfer_session_checkpoint_service_real_test PRIVATE
    "LINKER:--wrap=fflush" "LINKER:--wrap=fclose")
  set_tests_properties(ninfer_session_checkpoint_service_real_test PROPERTIES
    LABELS "gpu;real;durable" SKIP_RETURN_CODE 77 TIMEOUT 300 RUN_SERIAL TRUE)
endif()

ninfer_add_test(ninfer_session_checkpoint_store_test
  HOST_LANES bounded native windows
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_session_checkpoint_store.cpp"
  NEEDS_SOURCE_DIR
  LIBRARIES ninfer_session_checkpoint_store)
if(WIN32)
  target_link_libraries(ninfer_session_checkpoint_store_test PRIVATE advapi32)
  add_test(NAME ninfer_session_checkpoint_store_current_flush_test
    COMMAND ninfer_session_checkpoint_store_test current_flush)
  set_tests_properties(ninfer_session_checkpoint_store_current_flush_test
    PROPERTIES LABELS "host;durable" TIMEOUT 180)
  ninfer_test_host_lanes(ninfer_session_checkpoint_store_current_flush_test
    ninfer_session_checkpoint_store_test windows)
  add_test(NAME ninfer_session_checkpoint_store_protected_long_root_test
    COMMAND ninfer_session_checkpoint_store_test protected_long_root)
  set_tests_properties(ninfer_session_checkpoint_store_protected_long_root_test
    PROPERTIES LABELS "host;durable;elevated" TIMEOUT 180)
endif()
ninfer_add_test(ninfer_automatic_checkpoint_queue_test
  HOST_LANES bounded native windows
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_automatic_checkpoint_queue.cpp"
  NEEDS_SOURCE_DIR
  LIBRARIES ninfer_session_checkpoint_store)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(ninfer_session_checkpoint_store_test PRIVATE
    "LINKER:--wrap=fflush" "LINKER:--wrap=fclose")
  target_link_options(ninfer_automatic_checkpoint_queue_test PRIVATE
    "LINKER:--wrap=fflush" "LINKER:--wrap=fclose")
endif()
if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_test(NAME ninfer_session_checkpoint_store_capacity_test
    COMMAND ninfer_session_checkpoint_store_test disk_full_capacity)
  set_tests_properties(ninfer_session_checkpoint_store_capacity_test
    PROPERTIES LABELS "host;durable" TIMEOUT 180)
  ninfer_test_host_lanes(ninfer_session_checkpoint_store_capacity_test
    ninfer_session_checkpoint_store_test bounded native windows)
endif()
ninfer_add_test(ninfer_checkpoint_policy_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_checkpoint_policy.cpp")
set_tests_properties(ninfer_session_checkpoint_store_test ninfer_automatic_checkpoint_queue_test
  ninfer_checkpoint_policy_test PROPERTIES TIMEOUT 180)
set_tests_properties(ninfer_checkpoint_policy_test PROPERTIES LABELS "host")

ninfer_add_test(ninfer_anthropic_schema_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_anthropic_schema.cpp"
  LIBRARIES ninfer_serve)

ninfer_add_test(ninfer_serve_options_test
  HOST_LANES native
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_serve_options.cpp"
  LIBRARIES ninfer_serve)

ninfer_add_test(ninfer_request_log_test
  HOST_LANES native
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_request_log.cpp"
  LIBRARIES ninfer_serve)

ninfer_add_test(ninfer_operational_failure_test
  HOST_LANES bounded native windows
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_operational_failure.cpp"
          "${PROJECT_SOURCE_DIR}/src/serve/operational_log.cpp"
  LIBRARIES ninfer_product_logging)
# Exercise the real pure renderers without linking the unused Engine-capacity logger and
# its full CUDA model closure. Keep their transitive pretty-format/logging dependencies.
if(MSVC)
  target_compile_options(ninfer_operational_failure_test PRIVATE /Gy /Gw)
  target_link_options(ninfer_operational_failure_test PRIVATE /OPT:REF)
elseif(APPLE)
  target_link_options(ninfer_operational_failure_test PRIVATE LINKER:-dead_strip)
else()
  target_compile_options(ninfer_operational_failure_test PRIVATE -ffunction-sections -fdata-sections)
  target_link_options(ninfer_operational_failure_test PRIVATE LINKER:--gc-sections)
endif()

ninfer_add_test(ninfer_metrics_test
  HOST_LANES bounded native
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_metrics.cpp"
          ${PROJECT_SOURCE_DIR}/src/serve/metrics.cpp
  LIBRARIES ninfer::json Threads::Threads)

ninfer_add_test(ninfer_http_error_handler_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_http_error_handler.cpp"
  LIBRARIES ninfer_serve)

ninfer_add_test(ninfer_http_server_stop_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_http_server_stop.cpp"
  LIBRARIES ninfer_serve)

ninfer_add_test(ninfer_stop_event_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_stop_event.cpp"
  LIBRARIES ninfer_serve)
set_tests_properties(ninfer_stop_event_test PROPERTIES SKIP_RETURN_CODE 77)

ninfer_add_test(ninfer_http_transport_test
  HOST_LANES native
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_http_transport.cpp"
  LIBRARIES ninfer_serve)
