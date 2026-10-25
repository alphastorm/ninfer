ninfer_add_test(ninfer_media_decode_test
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

add_test(NAME ninfer_serve_restore_runner_test
  COMMAND ${Python3_EXECUTABLE} -B "${CMAKE_CURRENT_LIST_DIR}/../test_serve_restore_runner.py")
set_tests_properties(ninfer_serve_restore_runner_test PROPERTIES LABELS "host")

if(TARGET ninfer-serve)
  add_test(NAME ninfer_serve_restore_real_test
    COMMAND ${Python3_EXECUTABLE} -B "${CMAKE_CURRENT_LIST_DIR}/../test_serve_restore_real.py"
            --server $<TARGET_FILE:ninfer-serve>)
  set_tests_properties(ninfer_serve_restore_real_test PROPERTIES
    LABELS "gpu;real;durable" SKIP_RETURN_CODE 77 TIMEOUT 900 RUN_SERIAL TRUE)
endif()

ninfer_add_test(ninfer_session_checkpoint_store_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_session_checkpoint_store.cpp"
  LIBRARIES ninfer_session_checkpoint_store)
if(WIN32)
  target_link_libraries(ninfer_session_checkpoint_store_test PRIVATE advapi32)
  add_test(NAME ninfer_session_checkpoint_store_current_flush_test
    COMMAND ninfer_session_checkpoint_store_test current_flush)
  set_tests_properties(ninfer_session_checkpoint_store_current_flush_test
    PROPERTIES LABELS "host;durable" TIMEOUT 180)
  add_test(NAME ninfer_session_checkpoint_store_protected_long_root_test
    COMMAND ninfer_session_checkpoint_store_test protected_long_root)
  set_tests_properties(ninfer_session_checkpoint_store_protected_long_root_test
    PROPERTIES LABELS "host;durable;elevated" TIMEOUT 180)
endif()
ninfer_add_test(ninfer_automatic_checkpoint_queue_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_automatic_checkpoint_queue.cpp"
  LIBRARIES ninfer_session_checkpoint_store)
ninfer_add_test(ninfer_checkpoint_policy_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_checkpoint_policy.cpp")
set_tests_properties(ninfer_session_checkpoint_store_test ninfer_automatic_checkpoint_queue_test
  ninfer_checkpoint_policy_test PROPERTIES LABELS "host" TIMEOUT 180)

ninfer_add_test(ninfer_anthropic_schema_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_anthropic_schema.cpp"
  LIBRARIES ninfer_serve)

ninfer_add_test(ninfer_serve_options_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_serve_options.cpp"
  LIBRARIES ninfer_serve)

ninfer_add_test(ninfer_request_log_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_request_log.cpp"
  LIBRARIES ninfer_serve)

ninfer_add_test(ninfer_metrics_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_metrics.cpp"
  LIBRARIES ninfer_serve)

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
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_http_transport.cpp"
  LIBRARIES ninfer_serve)
