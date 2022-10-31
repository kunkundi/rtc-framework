project(signaling-server VERSION 0.3.0)
set(target signaling-server)

list(APPEND CMAKE_MODULE_PATH
	"${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/cmake"
)

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

target_compile_definitions(${target} PRIVATE BOOST_ALL_NO_LIB)

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
    set_target_properties(${target} PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:${target}>)
endif()

# copy test data
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    if(NOT EXISTS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG}/web/index.html")
        file(COPY "${CMAKE_SOURCE_DIR}/test_data/web" DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG})
        file(COPY "${CMAKE_SOURCE_DIR}/test_data/signaling-server.cfg" DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG})
    endif()

    if(NOT EXISTS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE}/web/index.html")
        file(COPY "${CMAKE_SOURCE_DIR}/test_data/web" DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE})
        file(COPY "${CMAKE_SOURCE_DIR}/test_data/signaling-server.cfg" DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE})
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(NOT EXISTS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/web/index.html")
        file(COPY "${CMAKE_SOURCE_DIR}/test_data/web" DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
        file(COPY "${CMAKE_SOURCE_DIR}/test_data/signaling-server.cfg" DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
    endif()
endif()

# copy runtime dlls
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
	add_custom_command(TARGET ${target}
		POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${CMAKE_SOURCE_DIR}/third_party/vtslog/lib/windows/$<CONFIG>/vtslog.dll" $<TARGET_FILE_DIR:${target}>
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${CMAKE_SOURCE_DIR}/third_party/Openssl-Win64/bin/libcrypto-1_1-x64.dll" $<TARGET_FILE_DIR:${target}>
	)
endif()
