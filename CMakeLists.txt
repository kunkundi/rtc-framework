cmake_minimum_required(VERSION 3.15)

project(rtc-solutions)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(signaling-server)
add_subdirectory(vtsrtc)
add_subdirectory(third_party)
