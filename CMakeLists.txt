cmake_minimum_required(VERSION 3.15)

project(rtc-solutions)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Use standalone Asio to remove the Boost.Asio dependency.
set(USE_STANDALONE_ASIO ON CACHE BOOL "Use standalone Asio instead of Boost.Asio" FORCE)

include(FetchContent)

find_package(spdlog CONFIG QUIET)

if(NOT TARGET spdlog::spdlog_header_only AND NOT TARGET spdlog::spdlog)
    if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/spdlog/CMakeLists.txt")
        set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
        set(SPDLOG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
        set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
        add_subdirectory("${CMAKE_SOURCE_DIR}/third_party/spdlog" "${CMAKE_BINARY_DIR}/third_party/spdlog")
    else()
        set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
        set(SPDLOG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
        set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            spdlog
            GIT_REPOSITORY https://github.com/gabime/spdlog.git
            GIT_TAG v1.14.1
        )
        FetchContent_MakeAvailable(spdlog)
    endif()
endif()

if(TARGET spdlog::spdlog_header_only)
    set(RTC_SPDLOG_TARGET spdlog::spdlog_header_only)
elseif(TARGET spdlog::spdlog)
    set(RTC_SPDLOG_TARGET spdlog::spdlog)
else()
    message(FATAL_ERROR "spdlog target is unavailable")
endif()

add_library(rtc_spdlog INTERFACE)
target_link_libraries(rtc_spdlog INTERFACE ${RTC_SPDLOG_TARGET})

find_path(RTC_ASIO_INCLUDE_DIR
    NAMES asio.hpp
    PATH_SUFFIXES include
    HINTS
        "${ASIO_PATH}"
        "$ENV{ASIO_PATH}"
        "$ENV{ASIO_ROOT}"
)

if(NOT RTC_ASIO_INCLUDE_DIR)
    FetchContent_Declare(
        asio
        GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
        GIT_TAG asio-1-32-0
        GIT_SUBMODULES ""
        GIT_SUBMODULES_RECURSE FALSE
    )
    FetchContent_GetProperties(asio)
    if(NOT asio_POPULATED)
        FetchContent_Populate(asio)
    endif()
    set(RTC_ASIO_INCLUDE_DIR "${asio_SOURCE_DIR}/asio/include")
endif()

set(ASIO_PATH "${RTC_ASIO_INCLUDE_DIR}" CACHE PATH "Path to standalone Asio headers" FORCE)

add_subdirectory(signaling-server)
add_subdirectory(vtsrtc)
add_subdirectory(third_party)
