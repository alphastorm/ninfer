target_sources(ninfer_ops PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/fp8_format.cpp"
)
if(CMAKE_CUDA_ARCHITECTURES STREQUAL "86")
  target_sources(ninfer_ops PRIVATE "${CMAKE_CURRENT_LIST_DIR}/unsupported_arch.cpp")
else()
  target_sources(ninfer_ops PRIVATE "${CMAKE_CURRENT_LIST_DIR}/fp8_a8.cu")
endif()
