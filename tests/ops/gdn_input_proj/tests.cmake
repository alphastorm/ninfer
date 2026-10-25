ninfer_add_op_test(ninfer_gdn_input_proj_conv_snapshot_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/test_snapshot.cpp"
  LIBRARIES ninfer_ops)
ninfer_add_op_test(ninfer_gdn_input_proj_conv_record_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/test_record.cpp"
  LIBRARIES ninfer_ops)
