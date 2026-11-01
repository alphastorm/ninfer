# Compile/link only: the double-build driver never executes CUDA code.
# Use the production archive policy, including its explicit device-link boundary.
add_library(ninfer_rdc_fixture STATIC
  ${CMAKE_CURRENT_LIST_DIR}/left/kernel.cu
  ${CMAKE_CURRENT_LIST_DIR}/right/kernel.cu)
ninfer_cuda_archive(ninfer_rdc_fixture)
add_executable(ninfer_rdc_determinism ${CMAKE_CURRENT_LIST_DIR}/main.cpp)
target_link_libraries(ninfer_rdc_determinism PRIVATE ninfer_rdc_fixture CUDA::cudart)
