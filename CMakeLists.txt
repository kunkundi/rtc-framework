cmake_minimum_required(VERSION 3.15)

project(rtc-solutions)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
SET(CMAKE_CXX_FLAGS_DEBUG "-O0")
SET(CMAKE_C_FLAGS_DEBUG "-O0")

add_subdirectory(signaling-server)
add_subdirectory(vtsrtc)
add_subdirectory(third_party)
