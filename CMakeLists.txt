cmake_minimum_required(VERSION 3.15)

project(rtc-solutions)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Use standalone Asio to remove the Boost.Asio dependency.
set(USE_STANDALONE_ASIO ON CACHE BOOL "Use standalone Asio instead of Boost.Asio" FORCE)

add_subdirectory(signaling-server)
add_subdirectory(vtsrtc)
add_subdirectory(third_party)
