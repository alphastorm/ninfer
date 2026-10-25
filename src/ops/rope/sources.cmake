target_sources(ninfer_ops PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/apply/rope.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/apply/dispatch.cu"
  "${CMAKE_CURRENT_LIST_DIR}/rmsnorm/rmsnorm_rope.cu"
)
