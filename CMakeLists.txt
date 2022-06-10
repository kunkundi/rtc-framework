cmake_minimum_required(VERSION 3.15)

project(rtc-solutions)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_FLAGS "-g")
set(CMAKE_EXE_LINKER_FLAGS "-g")
set(CMAKE_CXX_FLAGS_DEBUG "-Og")
set(CMAKE_C_FLAGS_DEBUG "-Og")

add_subdirectory(signaling-server)
add_subdirectory(vtsrtc)
add_subdirectory(third_party)
