ninfer_add_test(ninfer_host_decode_test
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../test_host_decode.cpp"
  LIBRARIES ninfer_core)
set_tests_properties(ninfer_host_decode_test PROPERTIES LABELS "host" TIMEOUT 30)

foreach(mode IN ITEMS baseline poll flush poll_flush)
  add_test(NAME ninfer_decode_round_${mode}_test COMMAND ninfer_device_test --round-wait)
  if(mode MATCHES "poll")
    set(poll 1)
  else()
    set(poll 0)
  endif()
  if(mode MATCHES "flush")
    set(flush 1)
  else()
    set(flush 0)
  endif()
  set_tests_properties(ninfer_decode_round_${mode}_test PROPERTIES
    ENVIRONMENT "NINFER_HOST_ROUND_POLL=${poll};NINFER_HOST_ROUND_FLUSH=${flush}"
    SKIP_RETURN_CODE 77 TIMEOUT 30)
endforeach()
