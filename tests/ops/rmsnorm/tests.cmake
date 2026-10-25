ninfer_add_op_test(ninfer_rmsnorm_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/test_rmsnorm.cpp"
  LIBRARIES ninfer_ops)
ninfer_add_op_test(ninfer_gated_rmsnorm_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/test_gated.cpp"
  LIBRARIES ninfer_ops)
