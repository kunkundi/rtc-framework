set(VTSLOG_ROOT_DIR ${CMAKE_SOURCE_DIR}/third_party/vtslog)

set(VTSLOG_INCLUDE_DIR ${VTSLOG_ROOT_DIR}/include)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  set(VTSLOG_LIBRARY
    debug ${VTSLOG_ROOT_DIR}/lib/windows/Debug/vtslog.lib
    optimized ${VTSLOG_ROOT_DIR}/lib/windows/Release/vtslog.lib
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "amd64")
    set(VTSLOG_LIBRARY ${VTSLOG_ROOT_DIR}/lib/linux/libvtslog.so)
  elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    set(VTSLOG_LIBRARY ${VTSLOG_ROOT_DIR}/lib/linux/libvtslog.aarch64.so)
  endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  message(STATUS "Configuring for macOS")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Vtslog 
  DEFAULT_MSG 
  VTSLOG_LIBRARY
  VTSLOG_INCLUDE_DIR
)
