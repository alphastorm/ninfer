target_sources(ninfer_ops PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/chunked/prepare.cu"
  "${CMAKE_CURRENT_LIST_DIR}/chunked/recurrence.cu"
  "${CMAKE_CURRENT_LIST_DIR}/kimi_delta_attention.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/plan.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/recurrent.cu"
)
