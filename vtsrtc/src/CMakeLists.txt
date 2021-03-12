project(vtsrtc VERSION 0.2.0)
set(target vtsrtc)

list(APPEND CMAKE_MODULE_PATH
	"${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/cmake"
)

find_package(Vtslog)
find_package(Boost REQUIRED)
find_package(WebRTC REQUIRED)

add_library(${target} SHARED
	log_manager.h
	log_manager.cpp
	http_status_code.hpp
	rtc_types.h
	rtc_device_manager.h
	rtc_device_manager.cpp
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
)

set_target_properties(${target} PROPERTIES DEBUG_POSTFIX d)

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

target_compile_definitions(${target}
PRIVATE
	BOOST_ALL_DYN_LINK
	BOOST_ALL_NO_LIB
)

target_include_directories(${target}
PUBLIC
	"$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>"
PRIVATE
	${VTSLOG_INCLUDE_DIR}
	${WEBRTC_INCLUDE_DIR}
)

target_link_libraries(${target}
PRIVATE
	${VTSLOG_LIBRARY}
	simple-web-server
	simple-websocket-server
	nlohmann_json::nlohmann_json
	${WEBRTC_LIBRARY}
)

set_target_properties(${target}
PROPERTIES
	VERSION ${PROJECT_VERSION}
	PUBLIC_HEADER "c_rtc.h"
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
		"${CMAKE_SOURCE_DIR}/third_party/vtslog/lib/windows/$<CONFIG>/libcurl.dll"
		"${CMAKE_SOURCE_DIR}/third_party/vtslog/lib/windows/$<CONFIG>/vtslog.dll"
		DESTINATION "${target}/bin/$<CONFIG>"
	)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	install(FILES "${CMAKE_SOURCE_DIR}/third_party/vtslog/lib/linux/libvtslog.so"
		DESTINATION "${target}/lib/$<CONFIG>"
	)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
	message(STATUS "TODO install for macOS")
endif()
