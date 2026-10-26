if(CMAKE_CUDA_ARCHITECTURES STREQUAL "120a")
  ninfer_add_test(ninfer_tma_descriptor_abi_test
    SOURCES "${CMAKE_CURRENT_LIST_DIR}/test_descriptor_abi.cu"
    LIBRARIES CUDA::cudart CUDA::cuda_driver)
  set_tests_properties(ninfer_tma_descriptor_abi_test PROPERTIES
    LABELS "gpu;cuda_core" SKIP_RETURN_CODE 77 TIMEOUT 60)
endif()
