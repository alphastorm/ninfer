find_package(CUDAToolkit REQUIRED)
find_package(Threads REQUIRED)
find_package(JPEG REQUIRED)

if(WIN32)
  # Use one CUDA runtime in libraries and CUDA-language executables alike.
  set(CMAKE_CUDA_RUNTIME_LIBRARY Shared)
  # vcpkg's hwloc port ships headers/libraries and pkg-config metadata, not a
  # CMake config package. Resolve its library through the selected toolchain.
  find_path(NINFER_HWLOC_INCLUDE_DIR hwloc.h REQUIRED)
  find_library(NINFER_HWLOC_LIBRARY NAMES hwloc libhwloc REQUIRED)
  add_library(ninfer_hwloc_dependencies INTERFACE)
  target_include_directories(ninfer_hwloc_dependencies INTERFACE ${NINFER_HWLOC_INCLUDE_DIR})
  target_link_libraries(ninfer_hwloc_dependencies INTERFACE ${NINFER_HWLOC_LIBRARY})
  set(NINFER_HWLOC_TARGET ninfer_hwloc_dependencies)

  find_package(FFMPEG REQUIRED)
  add_library(ninfer_ffmpeg_dependencies INTERFACE)
  target_include_directories(ninfer_ffmpeg_dependencies INTERFACE ${FFMPEG_INCLUDE_DIRS})
  target_link_directories(ninfer_ffmpeg_dependencies INTERFACE ${FFMPEG_LIBRARY_DIRS})
  target_link_libraries(ninfer_ffmpeg_dependencies INTERFACE ${FFMPEG_LIBRARIES})
  set(NINFER_FFMPEG_TARGET ninfer_ffmpeg_dependencies)
  set(NINFER_URING_TARGET "")
  include(${CMAKE_CURRENT_LIST_DIR}/DirectStorage.cmake)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(HWLOC REQUIRED IMPORTED_TARGET hwloc>=2.5)
  pkg_check_modules(URING REQUIRED IMPORTED_TARGET liburing>=2.5)
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat libavcodec libavutil libswscale)
  set(NINFER_HWLOC_TARGET PkgConfig::HWLOC)
  set(NINFER_URING_TARGET PkgConfig::URING)
  set(NINFER_FFMPEG_TARGET PkgConfig::FFMPEG)
endif()
set(NINFER_CUDART_TARGET CUDA::cudart)

if(MSVC)
  # Apply host flags to host compilation, never as bare /-options to nvcc.
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
  add_compile_options(
    "$<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>"
    "$<$<COMPILE_LANGUAGE:C,CXX>:/Zc:preprocessor>"
    "$<$<COMPILE_LANGUAGE:CXX>:/bigobj>"
    "$<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/utf-8,/Zc:preprocessor,/bigobj>")
endif()

# Repository-pinned header dependencies. No configure-time downloads.
add_library(ninfer::json INTERFACE IMPORTED GLOBAL)
target_include_directories(ninfer::json INTERFACE
  ${PROJECT_SOURCE_DIR}/third_party)

# Source base for the custom-template frontend; consumers will link it explicitly.
add_subdirectory(third_party/llama-jinja EXCLUDE_FROM_ALL)

# CPU grammar source base; consumers will link it explicitly.
add_subdirectory(third_party/xgrammar EXCLUDE_FROM_ALL)

if(NINFER_BUILD_PRODUCT_SUPPORT)
  # Media acquisition uses CURLOPT_PROTOCOLS_STR and CURLOPT_REDIR_PROTOCOLS_STR,
  # introduced in libcurl 7.85 (not merely the version of the maintainer environment).
  if(WIN32)
    find_package(CURL 7.85 REQUIRED)
    set(NINFER_CURL_TARGET CURL::libcurl)
  else()
    pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl>=7.85)
    set(NINFER_CURL_TARGET PkgConfig::LIBCURL)
  endif()
  add_library(ninfer::httplib INTERFACE IMPORTED GLOBAL)
  target_include_directories(ninfer::httplib INTERFACE
    ${PROJECT_SOURCE_DIR}/third_party/cpp-httplib)
  add_subdirectory(third_party/spdlog)
endif()
