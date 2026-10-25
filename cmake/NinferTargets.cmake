# Internal headers are private to each compile owner. External headers come from
# the dependency targets (or a component-local include for bundled C sources).
function(ninfer_internal_includes target)
  target_include_directories(${target} PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src)
  if(CMAKE_CUDA_ARCHITECTURES STREQUAL "86")
    target_compile_definitions(${target} PRIVATE NINFER_SM86=1)
  elseif(CMAKE_CUDA_ARCHITECTURES STREQUAL "89")
    target_compile_definitions(${target} PRIVATE NINFER_SM89=1)
  endif()
endfunction()

function(ninfer_cuda_archive target)
  set_target_properties(${target} PROPERTIES
    CUDA_SEPARABLE_COMPILATION ON
    CUDA_RESOLVE_DEVICE_SYMBOLS ON)
  target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:-lineinfo>)
endfunction()

function(ninfer_cuda_non_rdc_archive target)
  set_target_properties(${target} PROPERTIES
    CUDA_SEPARABLE_COMPILATION OFF
    CUDA_RESOLVE_DEVICE_SYMBOLS OFF)
  target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:-lineinfo>)
endfunction()
