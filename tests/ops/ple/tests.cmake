ninfer_add_op_test(ninfer_ple_gate_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/test_gate.cpp"
  LIBRARIES ninfer_ops)
ninfer_add_op_test(ninfer_ple_conv_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/test_conv.cpp"
  LIBRARIES ninfer_ops)
ninfer_add_op_test(ninfer_ple_state_extract_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/test_state_extract.cpp"
  LIBRARIES ninfer_ops)
