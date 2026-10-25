add_library(ninfer_gdn_control_test_support STATIC
  "${CMAKE_CURRENT_LIST_DIR}/fixtures.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/oracle.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/cases.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/runner.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/validation.cpp")
ninfer_test_includes(ninfer_gdn_control_test_support)
ninfer_op_oracle_options(ninfer_gdn_control_test_support)
target_link_libraries(ninfer_gdn_control_test_support PUBLIC ninfer_ops)
ninfer_add_op_test(ninfer_gdn_control_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/test_control.cpp"
  LIBRARIES ninfer_gdn_control_test_support)
ninfer_add_op_test(ninfer_gdn_control_proj_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/test_projection.cpp"
  LIBRARIES ninfer_gdn_control_test_support)
ninfer_add_op_test(ninfer_gdn_norm_control_proj_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/test_norm_projection.cpp"
  LIBRARIES ninfer_gdn_control_test_support)
