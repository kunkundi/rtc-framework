set(NVCODEC_ROOT_DIR ${CMAKE_SOURCE_DIR}/third_party/Video_Codec_SDK_11.0.10)

set(NVCODEC_INCLUDE_DIR
  ${NVCODEC_ROOT_DIR}/Interface
  ${NVCODEC_ROOT_DIR}/Samples/NvCodec
  ${NVCODEC_ROOT_DIR}/Samples/Utils
)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  set(NVCODEC_LIBRARY ${NVCODEC_ROOT_DIR}/Lib/x64/nvcuvid.lib
    ${NVCODEC_ROOT_DIR}/Lib/x64/nvencodeapi.lib
  )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(NVCODEC_LIBRARY ${NVCODEC_ROOT_DIR}/Lib/linux/stubs/x86_64/libnvcuvid.so
    ${NVCODEC_ROOT_DIR}/Lib/linux/stubs/x86_64/libnvidia-encode.so
  )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
	message(STATUS "Configuring for macOS")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Nvcodec
  DEFAULT_MSG
  NVCODEC_LIBRARY
  NVCODEC_INCLUDE_DIR
)
