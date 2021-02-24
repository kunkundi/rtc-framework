project(librtc_examples)

set(target p2p_qt)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

list(APPEND CMAKE_PREFIX_PATH ${QTDIR})

find_package(Qt5Core)
find_package(Qt5Widgets)
find_package(Qt5Gui)
find_package(Qt5OpenGL)

add_executable(${target}
	rtc_videorender.h
	rtc_videorender.cpp
	rtc_widget.h
	rtc_widget.cpp
	main.cpp
)

target_link_libraries(${target}
	librtc
	Qt5::Core
	Qt5::Widgets
	Qt5::Gui
	Qt5::OpenGL
)
