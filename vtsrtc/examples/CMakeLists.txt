project(vtsrtc_examples)

set(target p2p_qt)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

list(APPEND CMAKE_PREFIX_PATH ${QTDIR})

find_package(Qt5Core)
find_package(Qt5Widgets)
find_package(Qt5Gui)
find_package(Qt5OpenGL REQUIRED)
find_package(Threads REQUIRED)

if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
	add_executable(${target}
		rtc_audiorender.h
		rtc_audiorender.cpp
		rtc_widget.h
		rtc_widget.cpp
		main.cpp
	)
else()
	add_executable(${target}
		rtc_audiorender.h
		rtc_audiorender.cpp
		rtc_videorender.h
		rtc_videorender.cpp
		rtc_widget.h
		rtc_widget.cpp
		main.cpp
	)
endif()

if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
target_link_libraries(${target}
	${OPENGL_LIBRARIES}
	vtsrtc.aarch64
	Qt5::Core
	Qt5::Widgets
	Qt5::Gui
	Qt5::OpenGL
	Threads::Threads
)
else()
target_link_libraries(${target}
	${OPENGL_LIBRARIES}
	vtsrtc
	Qt5::Core
	Qt5::Widgets
	Qt5::Gui
	Qt5::OpenGL
	Threads::Threads
)
endif()

if(MSVC)
    set_target_properties(${target} PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:${target}>)
endif()

# copy test data
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
	if(NOT EXISTS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG}/rtc.cfg")
		file(COPY "${CMAKE_SOURCE_DIR}/test_data/rtc.cfg" DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG})
		file(COPY "${CMAKE_SOURCE_DIR}/test_data/8k16bit.pcm" DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG})
		file(COPY "${CMAKE_SOURCE_DIR}/test_data/yuvfiles" DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG})
	endif()

	if(NOT EXISTS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE}/rtc.cfg")
		file(COPY "${CMAKE_SOURCE_DIR}/test_data/rtc.cfg" DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE})
		file(COPY "${CMAKE_SOURCE_DIR}/test_data/8k16bit.pcm" DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE})
		file(COPY "${CMAKE_SOURCE_DIR}/test_data/yuvfiles" DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE})
	endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	if(NOT EXISTS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/rtc.cfg")
		file(COPY "${CMAKE_SOURCE_DIR}/test_data/rtc.cfg" DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
		file(COPY "${CMAKE_SOURCE_DIR}/test_data/8k16bit.pcm" DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
		file(COPY "${CMAKE_SOURCE_DIR}/test_data/yuvfiles" DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
	endif()
endif()

# copy runtime dlls
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
	add_custom_command(TARGET ${target}
		POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:vtsrtc> $<TARGET_FILE_DIR:${target}>
		COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${target}>/platforms
		COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:Qt5::QWindowsIntegrationPlugin> $<TARGET_FILE_DIR:${target}>/platforms
		COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:Qt5::Core> $<TARGET_FILE_DIR:${target}>
		COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:Qt5::Widgets> $<TARGET_FILE_DIR:${target}>
		COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:Qt5::Gui> $<TARGET_FILE_DIR:${target}>
		COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:Qt5::OpenGL> $<TARGET_FILE_DIR:${target}>
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${CMAKE_SOURCE_DIR}/third_party/vtslog/lib/windows/$<CONFIG>/libcurl.dll" $<TARGET_FILE_DIR:${target}>
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${CMAKE_SOURCE_DIR}/third_party/vtslog/lib/windows/$<CONFIG>/vtslog.dll" $<TARGET_FILE_DIR:${target}>
	)
endif()
