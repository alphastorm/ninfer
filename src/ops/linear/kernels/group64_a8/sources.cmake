# Native sm_86/sm_89 Group64 A8 prefill kernels exist only in builds that opt in.
if(NINFER_NATIVE_PREFILL_GROUP64_A8)
  target_sources(ninfer_ops PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/launch.cu"
  )
endif()
