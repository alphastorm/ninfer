add_library(ninfer_checkpoint_contract STATIC contract/checkpoint_io.cpp)
ninfer_internal_includes(ninfer_checkpoint_contract)
target_link_libraries(ninfer_checkpoint_contract PUBLIC Threads::Threads ninfer_sha256)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_library(ninfer_checkpoint_io_uring_linux STATIC
    platform/linux/io_uring_checkpoint_backend.cpp)
  ninfer_internal_includes(ninfer_checkpoint_io_uring_linux)
  target_link_libraries(ninfer_checkpoint_io_uring_linux PUBLIC ninfer_checkpoint_contract
    PRIVATE ninfer_durable_io)
endif()

if(WIN32)
  add_library(ninfer_checkpoint_directstorage_windows STATIC
    windows/direct_storage_checkpoint_backend.cpp
    windows/direct_storage_checkpoint_windows.cpp)
  ninfer_internal_includes(ninfer_checkpoint_directstorage_windows)
  target_link_libraries(ninfer_checkpoint_directstorage_windows
    PUBLIC ninfer_checkpoint_contract
    PRIVATE ninfer_durable_io ninfer_directstorage ${NINFER_CUDART_TARGET})
endif()

add_library(ninfer_runtime_support STATIC
  engine/context_cache/context_cost.cpp
  engine/context_cache/context_cost_defaults.cpp
  engine/kv_capacity.cpp
  engine/startup_memory.cpp
  engine/public_types.cpp
)
ninfer_internal_includes(ninfer_runtime_support)
target_link_libraries(ninfer_runtime_support PUBLIC ninfer_core PRIVATE ninfer::json)

# Public Engine and common contracts; model execution remains in the model layer.
add_library(ninfer_engine STATIC
  engine/model_construction.cpp
  contract/sampling.cpp
  engine/engine.cpp
)
ninfer_internal_includes(ninfer_engine)
target_include_directories(ninfer_engine PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(ninfer_engine PRIVATE
  ninfer_model_runtime
  ninfer_artifact
  ninfer_core
  ninfer_runtime_support
  ninfer_ops
  ninfer_text
  ninfer_media_decode)
add_library(ninfer::engine ALIAS ninfer_engine)
