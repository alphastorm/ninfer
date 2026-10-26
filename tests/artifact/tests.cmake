ninfer_add_test(ninfer_artifact_reader_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/test_reader.cpp"
  LIBRARIES ninfer_artifact)

add_test(NAME ninfer_artifact_file_io_test
  COMMAND ${Python3_EXECUTABLE} -B "${CMAKE_CURRENT_LIST_DIR}/test_file_io.py")
set_tests_properties(ninfer_artifact_file_io_test PROPERTIES TIMEOUT 30 LABELS "host")

ninfer_add_test(ninfer_artifact_backing_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/test_backing.cpp"
  LIBRARIES ninfer_artifact)
set_tests_properties(ninfer_artifact_backing_test PROPERTIES TIMEOUT 30)

ninfer_add_test(ninfer_artifact_materialization_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/test_materialization.cpp" "${CMAKE_CURRENT_LIST_DIR}/materialization_io.cpp"
  LIBRARIES ninfer_artifact)
ninfer_cuda_test_interposition(ninfer_artifact_materialization_test)

if(NOT WIN32)
target_link_options(ninfer_artifact_materialization_test PRIVATE
  "LINKER:--wrap=cudaMalloc"
  "LINKER:--wrap=cudaMallocHost"
  "LINKER:--wrap=cudaFree"
  "LINKER:--wrap=cudaFreeHost"
  "LINKER:--wrap=cudaEventCreateWithFlags"
  "LINKER:--wrap=cudaEventRecord"
  "LINKER:--wrap=cudaMemcpyAsync"
  "LINKER:--wrap=cudaStreamSynchronize"
  "LINKER:--wrap=statx")
endif()

add_test(NAME ninfer_artifact_writer_interop_test
  COMMAND ${Python3_EXECUTABLE} -B "${CMAKE_CURRENT_LIST_DIR}/writer_interop.py"
    $<TARGET_FILE:ninfer_artifact_materialization_test>)

set_tests_properties(
  ninfer_artifact_writer_interop_test
  PROPERTIES SKIP_RETURN_CODE 77)

set_tests_properties(
  ninfer_artifact_materialization_test
  PROPERTIES SKIP_RETURN_CODE 77)

add_test(NAME ninfer_artifact_cpu_materialization_test
  COMMAND ninfer_artifact_materialization_test --cpu-only)
set_tests_properties(ninfer_artifact_cpu_materialization_test
  PROPERTIES ENVIRONMENT "CUDA_VISIBLE_DEVICES=")
