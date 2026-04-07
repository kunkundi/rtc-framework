project(vtsrtc_examples)

set(target p2p_qt)
set(vtsrtc_lib_target vtsrtc)
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    set(vtsrtc_lib_target vtsrtc.aarch64)
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

if(DEFINED QTDIR AND NOT "${QTDIR}" STREQUAL "")
    list(APPEND CMAKE_PREFIX_PATH "${QTDIR}")
endif()

find_package(Qt5 QUIET COMPONENTS Core Widgets Gui OpenGL)
if(NOT TARGET Qt5::Core OR NOT TARGET Qt5::Widgets OR NOT TARGET Qt5::Gui OR NOT TARGET Qt5::OpenGL)
    message(WARNING "Qt5 was not found. Skipping vtsrtc examples target p2p_qt.")
    return()
endif()

find_package(Threads REQUIRED)

add_executable(${target}
    rtc_audiorender.h
    rtc_audiorender.cpp
    rtc_videorender.h
    rtc_videorender.cpp
    rtc_widget.h
    rtc_widget.cpp
    main.cpp
)

target_link_libraries(${target}
    ${OPENGL_LIBRARIES}
    ${vtsrtc_lib_target}
    Qt5::Core
    Qt5::Widgets
    Qt5::Gui
    Qt5::OpenGL
    Threads::Threads
)

if(MSVC)
    set_target_properties(${target} PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:${target}>)
endif()

add_custom_command(TARGET ${target}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/test_data/rtc.cfg" "$<TARGET_FILE_DIR:${target}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/test_data/8k16bit.pcm" "$<TARGET_FILE_DIR:${target}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/test_data/zjlabs.yuv" "$<TARGET_FILE_DIR:${target}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/test_data/messagefile.txt" "$<TARGET_FILE_DIR:${target}>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/test_data/yuvfiles" "$<TARGET_FILE_DIR:${target}>/yuvfiles"
)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    add_custom_command(TARGET ${target}
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:${vtsrtc_lib_target}>" "$<TARGET_FILE_DIR:${target}>"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:${target}>/platforms"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt5::QWindowsIntegrationPlugin>" "$<TARGET_FILE_DIR:${target}>/platforms"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt5::Core>" "$<TARGET_FILE_DIR:${target}>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt5::Widgets>" "$<TARGET_FILE_DIR:${target}>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt5::Gui>" "$<TARGET_FILE_DIR:${target}>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt5::OpenGL>" "$<TARGET_FILE_DIR:${target}>"
    )
endif()
