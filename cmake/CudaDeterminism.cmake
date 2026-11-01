# Preserve caller-selected caches, but put the stable seed/temp-name policy in
# front of them. Docker, presets and the release build wrapper all enter here.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CUDA_COMPILER_ID STREQUAL "NVIDIA")
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  list(PREPEND CMAKE_CUDA_COMPILER_LAUNCHER
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tools/release/deterministic-nvcc.py"
    --source-root "${PROJECT_SOURCE_DIR}" --)
endif()
