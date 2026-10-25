target_sources(ninfer_ops PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/pack_tail.cu"
  "${CMAKE_CURRENT_LIST_DIR}/rmsnorm.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/operands.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/dispatch.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/launch.cu")
