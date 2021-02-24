project(signaling-server VERSION 0.1.0)
set(target signaling-server)

list(APPEND CMAKE_MODULE_PATH
	"${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/cmake"
)

add_definitions(-DBOOST_ALL_DYN_LINK -DBOOST_ALL_NO_LIB)

find_package(Vtslog)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(OPENSSL_ROOT_DIR "${CMAKE_SOURCE_DIR}/third_party/OpenSSL-Win64")
endif()
find_package(OpenSSL REQUIRED)
find_package(Boost REQUIRED COMPONENTS filesystem)

add_executable(${target}
    log_manager.h
    log_manager.cpp
    http_status_code.hpp
    http_controller.h
    http_controller.cpp
    ws_controller.h
    ws_controller.cpp
    main.cpp
)

target_include_directories(${target}
PRIVATE
    ${VTSLOG_INCLUDE_DIR}
    ${OPENSSL_INCLUDE_DIR}
)

target_link_libraries(${target}
    ${VTSLOG_LIBRARY}
    nlohmann_json::nlohmann_json
    ${OPENSSL_LIBRARIES}
    simple-web-server
    simple-websocket-server
    Boost::filesystem
)

if(MSVC)
    set_target_properties(${target} PROPERTIES 
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>")
endif()
