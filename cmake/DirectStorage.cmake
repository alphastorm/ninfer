# The native checkpoint backend uses the shipped DirectStorage 1.3.0 package.
# FETCHCONTENT_SOURCE_DIR_DIRECTSTORAGE can point at a pre-extracted offline copy.
include(FetchContent)
FetchContent_Declare(directstorage
  URL "https://www.nuget.org/api/v2/package/Microsoft.Direct3D.DirectStorage/1.3.0"
  URL_HASH SHA256=8ab6c1082537565388b79050d94fdd7f5f437cd0f1d909e7b19f786042b97177
  DOWNLOAD_NAME "directstorage.1.3.0.zip"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(directstorage)
add_library(ninfer_directstorage INTERFACE)
target_include_directories(ninfer_directstorage INTERFACE "${directstorage_SOURCE_DIR}/native/include")
target_link_libraries(ninfer_directstorage INTERFACE
  "${directstorage_SOURCE_DIR}/native/lib/x64/dstorage.lib" d3d12 dxgi)
set(NINFER_DIRECTSTORAGE_DLLS
  "${directstorage_SOURCE_DIR}/native/bin/x64/dstorage.dll"
  "${directstorage_SOURCE_DIR}/native/bin/x64/dstoragecore.dll")
