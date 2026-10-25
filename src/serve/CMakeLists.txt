add_library(ninfer_serve_state STATIC
  client_identity.cpp
  checkpoint_identity.cpp
  openai_responses_store.cpp)
ninfer_internal_includes(ninfer_serve_state)
target_link_libraries(ninfer_serve_state PUBLIC ninfer_sha256 ninfer::json Threads::Threads)

add_library(ninfer_session_checkpoint_store STATIC
  session_checkpoint_store.cpp
  automatic_checkpoint_queue.cpp)
ninfer_internal_includes(ninfer_session_checkpoint_store)
target_link_libraries(ninfer_session_checkpoint_store
  PUBLIC ninfer_serve_state ninfer_checkpoint_contract ninfer_durable_io Threads::Threads)

add_library(ninfer_serve STATIC
  opaque_id.cpp
  stop_event.cpp
  server_identity.cpp
  http_transport.cpp
  request_validation.cpp
  openai_chat_request.cpp
  openai_chat_response.cpp
  openai_chat_http.cpp
  openai_common.cpp
  openai_responses_request.cpp
  openai_responses_state.cpp
  openai_responses_response.cpp
  anthropic_messages_request.cpp
  anthropic_messages_response.cpp
  anthropic_messages_http.cpp
  translate.cpp
  generation_service.cpp
  request_events.cpp
  operational_log.cpp
  request_log.cpp
  metrics.cpp
  http_server.cpp
  openai_responses_http.cpp
  serve_options.cpp
)
ninfer_internal_includes(ninfer_serve)
target_link_libraries(ninfer_serve
  PUBLIC ninfer_engine ninfer_serve_state Threads::Threads ninfer::json ninfer::httplib
  PRIVATE ninfer_media_acquire ninfer_product_logging ninfer_grammar ninfer_sha256
          ninfer_session_checkpoint_store CUDA::cudart)
if(WIN32)
  target_link_libraries(ninfer_serve PRIVATE bcrypt advapi32)
endif()
