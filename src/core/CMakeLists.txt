if(NOT (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR WIN32) OR
   NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  message(FATAL_ERROR "NInfer CPU execution requires Linux or Windows x86-64")
endif()

add_library(ninfer_cpu STATIC
  cpu/system.cpp
  cpu/task_pool.cpp
  cpu/executor.cpp
)
ninfer_internal_includes(ninfer_cpu)
target_link_libraries(ninfer_cpu PRIVATE ${NINFER_HWLOC_TARGET} Threads::Threads)

add_library(ninfer_durable_io STATIC io/durable_file.cpp)
ninfer_internal_includes(ninfer_durable_io)

if(WIN32)
  set(ninfer_platform_io_sources io/file_windows.cpp io/read_queue_windows.cpp)
else()
  set(ninfer_platform_io_sources io/file_posix.cpp io/read_queue_linux.cpp)
endif()

add_library(ninfer_sha256 STATIC sha256.cpp sha256_x86.cpp)
ninfer_internal_includes(ninfer_sha256)

add_library(ninfer_core STATIC
  arena.cpp
  buffer.cpp
  buffer.cu
  memory.cpp
  transfer.cpp
  transfer.cu
  device.cu
  gpu_keep_warm.cu
  dtype.cpp
  nf4.cpp
  io/file.cpp
  ${ninfer_platform_io_sources}
  gdn_replay_records.cpp
  cyclic_kv_cache.cpp
  host_context_arena.cpp
  host_kv_arena.cpp
  paged_kv_cache.cpp
  linear_attention_state.cpp
  state_image.cpp
  layout.cpp
  tensor.cpp
  weight_view.cpp
  weight_bank.cpp
  expert_cache.cpp
  decode_graph.cpp
)
ninfer_internal_includes(ninfer_core)
ninfer_cuda_archive(ninfer_core)
target_link_libraries(ninfer_core PUBLIC ninfer_cpu ${NINFER_CUDART_TARGET} CUDA::nvtx3 Threads::Threads)
target_link_libraries(ninfer_core PRIVATE ${NINFER_URING_TARGET})
