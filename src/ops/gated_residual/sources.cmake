target_sources(ninfer_ops PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/norm_combine.cu"
  "${CMAKE_CURRENT_LIST_DIR}/operands.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/mix/mix.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/mix/operands.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/mix/select.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/mix/launch.cu")
