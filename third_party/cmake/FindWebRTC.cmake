# Find WebRTC include path
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
	set(WEBRTC_DIR "${CMAKE_SOURCE_DIR}/third_party/webrtc/webrtc-win")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    if(USE_DEFAULT_JETSON_ENCODER)
      message(STATUS "Use default jetson encoder")
      set(WEBRTC_DIR "${CMAKE_SOURCE_DIR}/third_party/webrtc/webrtc-jetson-default")
    else()
      set(WEBRTC_DIR "${CMAKE_SOURCE_DIR}/third_party/webrtc/webrtc-jetson")
    endif()
  else()
    set(WEBRTC_DIR "${CMAKE_SOURCE_DIR}/third_party/webrtc/webrtc-linux")
  endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
	message(STATUS "Configuring for macOS")
endif()

set(WEBRTC_INCLUDE_DIR
  ${WEBRTC_DIR}/include
  ${WEBRTC_DIR}/include/third_party/abseil-cpp
  ${WEBRTC_DIR}/include/third_party/jsoncpp/source/include
  ${WEBRTC_DIR}/include/third_party/jsoncpp/generated
  ${WEBRTC_DIR}/include/third_party/libyuv/include
  ${WEBRTC_DIR}/include/third_party/boringssl/src/include
)

set(WEBRTC_OBJC_INCLUDE_DIR
  ${WEBRTC_DIR}/include/sdk/objc
  ${WEBRTC_DIR}/include/sdk/objc/base
)

set(WEBRTC_LIBRARY_DIR
  ${WEBRTC_DIR}/lib
)

if(NOT USE_DEFAULT_JETSON_ENCODER)
  find_library(WEBRTC_LIBRARY_DEBUG
    NAMES webrtcd
    PATHS ${WEBRTC_LIBRARY_DIR}
  )
endif()

find_library(WEBRTC_LIBRARY_RELEASE
  NAMES webrtc
  PATHS ${WEBRTC_LIBRARY_DIR}
)

if(USE_DEFAULT_JETSON_ENCODER)
set(WEBRTC_LIBRARY
optimized ${WEBRTC_LIBRARY_RELEASE}
)
else()
set(WEBRTC_LIBRARY
debug ${WEBRTC_LIBRARY_DEBUG}
optimized ${WEBRTC_LIBRARY_RELEASE}
)
endif()

if(NOT USE_DEFAULT_JETSON_ENCODER)
  message(${WEBRTC_LIBRARY_DEBUG} )
endif()
message(${WEBRTC_LIBRARY_RELEASE})
message(${WEBRTC_LIBRARY})

include(FindPackageHandleStandardArgs)
if(USE_DEFAULT_JETSON_ENCODER)
find_package_handle_standard_args(WebRTC 
  DEFAULT_MSG 
  WEBRTC_LIBRARY
  WEBRTC_LIBRARY_RELEASE 
  WEBRTC_INCLUDE_DIR
)
else()
find_package_handle_standard_args(WebRTC 
  DEFAULT_MSG 
  WEBRTC_LIBRARY
  WEBRTC_LIBRARY_DEBUG
  WEBRTC_LIBRARY_RELEASE 
  WEBRTC_INCLUDE_DIR
)
endif()