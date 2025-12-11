project(vtsrtc VERSION 0.5.0)
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
        set(target vtsrtc.aarch64)
else()
	set(target vtsrtc)
endif()

if(USE_DEFAULT_JETSON_ENCODER)
    add_definitions(-DUSE_DEFAULT_JETSON_ENCODER)
endif(USE_DEFAULT_JETSON_ENCODER)

# 编码性能统计选项（默认关闭，生产环境建议关闭）
option(ENABLE_ENCODE_PERF_STATS "Enable encode performance statistics" OFF)

list(APPEND CMAKE_MODULE_PATH
	"${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/cmake"
)

if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
	message(STATUS "Build for Aarch64")
endif()

find_package(Vtslog)
find_package(Boost REQUIRED)
find_package(WebRTC REQUIRED)

if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
	message(STATUS "Use hardware accelerated codec on Aarch64")
else()
	message(STATUS "Use Nvcodec")
	find_package(CUDAToolkit REQUIRED)
	find_package(Nvcodec REQUIRED)
endif()

list(APPEND SOURCE_CODE
	log/log_manager.h
	log/log_manager.cpp
	log/log_webrtc_hook.h
	log/log_webrtc_hook.cpp
	http_status_code.hpp
	rtc_types.h
	rtc_device_manager.h
	rtc_device_manager.cpp
	rtc_audiosink.hpp
	rtc_audiosource.hpp
	rtc_videosource.hpp
	rtc_videocapturer.hpp
	rtc_videosink.hpp
	observer.hpp
	rtc_connection.h
	rtc_connection.cpp
	rtc_connection_manager.h
	rtc_connection_manager.cpp
	rtc.h
	rtc.cpp
	c_rtc.h
	c_rtc.cpp
	video/encode/rtc_encoder_factory.h
	video/encode/rtc_encoder_factory.cpp
	statistics/rtc_statistics.h
	statistics/rtc_statistics.cpp
	packet/pack.h
	packet/pack.cpp
	packet/packtype.h
	packet/semaphore.h
	packet/squeue.h
	packet/buffer.h
	packet/packetizer.h
	packet/packetizer.cpp
	packet/depacketizer.h
	packet/depacketizer.cpp

)

if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
	list(APPEND SOURCE_CODE
		video/encode/nvidia-jetson/jetsonh264_encoder_impl.h
		video/encode/nvidia-jetson/jetsonh264_encoder_impl.cpp
		video/encode/nvidia-jetson/jetson_encoder.h
		video/encode/nvidia-jetson/jetson_encoder.cpp
		video/encode/nvidia-jetson/nvmpi.h
		video/encode/nvidia-jetson/nvmpi_enc.cpp
		# video/encode/nvidia-jetson/rtc_codec_pool.h
		# video/encode/nvidia-jetson/rtc_codec_pool.cpp
		/usr/src/jetson_multimedia_api/include/NvVideoEncoder.h
		/usr/src/jetson_multimedia_api/samples/common/classes/NvVideoEncoder.cpp
		/usr/src/jetson_multimedia_api/include/NvV4l2Element.h
		/usr/src/jetson_multimedia_api/samples/common/classes/NvV4l2Element.cpp
		/usr/src/jetson_multimedia_api/include/NvV4l2ElementPlane.h
		/usr/src/jetson_multimedia_api/samples/common/classes/NvV4l2ElementPlane.cpp
		/usr/src/jetson_multimedia_api/include/NvElementProfiler.h
		/usr/src/jetson_multimedia_api/samples/common/classes/NvElementProfiler.cpp
		/usr/src/jetson_multimedia_api/include/NvBuffer.h
		/usr/src/jetson_multimedia_api/samples/common/classes/NvBuffer.cpp
		/usr/src/jetson_multimedia_api/include/NvElement.h
		/usr/src/jetson_multimedia_api/samples/common/classes/NvElement.cpp
		/usr/src/jetson_multimedia_api/include/NvLogging.h
		/usr/src/jetson_multimedia_api/samples/common/classes/NvLogging.cpp
		/usr/src/jetson_multimedia_api/include/libv4l2.h
	)
else()
	list(APPEND SOURCE_CODE
		video/decode/rtc_decoder_factory.h
		video/decode/rtc_decoder_factory.cpp
		video/encode/nvidia/nvh264_encoder_impl.h
		video/encode/nvidia/nvh264_encoder_impl.cpp
		video/decode/nvidia/nvh264_decoder_impl.h
		video/decode/nvidia/nvh264_decoder_impl.cpp
		${CMAKE_SOURCE_DIR}/third_party/Video_Codec_SDK_11.0.10/Samples/NvCodec/NvEncoder/NvEncoder.cpp
		${CMAKE_SOURCE_DIR}/third_party/Video_Codec_SDK_11.0.10/Samples/NvCodec/NvEncoder/NvEncoderCuda.cpp
		${CMAKE_SOURCE_DIR}/third_party/Video_Codec_SDK_11.0.10/Samples/NvCodec/NvDecoder/NvDecoder.cpp
	)
endif()

add_library(${target} SHARED ${SOURCE_CODE})

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
	target_compile_definitions(${target}
    PUBLIC
		WEBRTC_WIN
		NOMINMAX
		WIN32_LEAN_AND_MEAN
	PRIVATE
		RTC_DLL_EXPORTS
	)
	target_link_libraries(${target}
	PRIVATE
		winmm
		Secur32
		Msdmo
		Dmoguids
		wmcodecdspuuid
		Strmiids
	)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	target_compile_definitions(${target}
	PUBLIC
		WEBRTC_LINUX
		WEBRTC_POSIX
	)
	# enable -fPIC option
	set_target_properties(${target}
	PROPERTIES
		POSITION_INDEPENDENT_CODE ON
	)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
	message(STATUS "TODO configure for macOS")
endif()

target_compile_definitions(${target} PRIVATE BOOST_ALL_NO_LIB)

if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
	target_include_directories(${target}
	PUBLIC
		"$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>"
	PRIVATE
		${VTSLOG_INCLUDE_DIR}
		${WEBRTC_INCLUDE_DIR}
		"/usr/src/jetson_multimedia_api/include"
	)
	
	# 设置编码性能统计编译定义
	if(ENABLE_ENCODE_PERF_STATS)
		target_compile_definitions(${target} PRIVATE ENABLE_ENCODE_PERF_STATS=1)
		message(STATUS "编码性能统计: 已启用")
	else()
		target_compile_definitions(${target} PRIVATE ENABLE_ENCODE_PERF_STATS=0)
		message(STATUS "编码性能统计: 已关闭（生产环境推荐）")
	endif()
else()
	target_include_directories(${target}
	PUBLIC
		"$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>"
	PRIVATE
		${VTSLOG_INCLUDE_DIR}
		${CUDAToolkit_INCLUDE_DIRS}
		${NVCODEC_INCLUDE_DIR}
		${WEBRTC_INCLUDE_DIR}
	)
endif()

if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
	target_link_libraries(${target}
	PRIVATE
		${VTSLOG_LIBRARY}
		simple-web-server
		simple-websocket-server
		nlohmann_json::nlohmann_json
		${WEBRTC_LIBRARY}
		v4l2
		nvbufsurface
		nvbufsurftransform
		X11
		-L"/usr/lib/aarch64-linux-gnu/tegra/"
	)
else()
	target_link_libraries(${target}
	PRIVATE
		${VTSLOG_LIBRARY}
		CUDA::cuda_driver
		${NVCODEC_LIBRARY}
		simple-web-server
		simple-websocket-server
		nlohmann_json::nlohmann_json
		${WEBRTC_LIBRARY}
	)
endif()

set_target_properties(${target}
PROPERTIES
	VERSION ${PROJECT_VERSION}
	DEBUG_POSTFIX "d"
	PUBLIC_HEADER "c_rtc.h"
	WINDOWS_EXPORT_ALL_SYMBOLS ON
)

# install library
install(TARGETS ${target}
	LIBRARY DESTINATION "${target}/lib/$<CONFIG>"
	ARCHIVE DESTINATION "${target}/lib/$<CONFIG>"
	RUNTIME DESTINATION "${target}/bin/$<CONFIG>"
	PUBLIC_HEADER DESTINATION "${target}/include"
)

install(FILES "${CMAKE_SOURCE_DIR}/test_data/rtc.cfg" DESTINATION ${target})

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
	install(FILES
		"${CMAKE_SOURCE_DIR}/third_party/vtslog/lib/windows/$<CONFIG>/vtslog.dll"
		DESTINATION "${target}/bin/$<CONFIG>"
	)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
		install(FILES 
			"${VTSLOG_LIBRARY}"
			DESTINATION "${target}/lib/$<CONFIG>"
		)
	elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
		install(FILES 
		"${VTSLOG_LIBRARY}"
		"${NVCODEC_LIBRARY}"
		DESTINATION "${target}/lib/$<CONFIG>"
		)
	endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
	message(STATUS "TODO install for macOS")
endif()
