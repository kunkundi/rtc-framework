project(signaling-server VERSION 0.2.0)
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
    set_target_properties(${target} PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:${target}>)
endif()

# copy test data
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    if(NOT EXISTS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG}/web/index.html")
        file(COPY "${CMAKE_SOURCE_DIR}/test_data/web" DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG})
    endif()

    if(NOT EXISTS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE}/web/index.html")
        file(COPY "${CMAKE_SOURCE_DIR}/test_data/web" DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE})
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(NOT EXISTS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/web/index.html")
        file(COPY "${CMAKE_SOURCE_DIR}/test_data/web" DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
    endif()
endif()

# copy runtime dlls
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    get_filename_component(BOOSTFS_DEBUG_BASENAME ${Boost_FILESYSTEM_LIBRARY_DEBUG} NAME_WE)
    get_filename_component(BOOSTFS_DEBUG_PATH ${Boost_FILESYSTEM_LIBRARY_DEBUG} PATH)
    file(COPY "${BOOSTFS_DEBUG_PATH}/${BOOSTFS_DEBUG_BASENAME}.dll" DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG})

    get_filename_component(BOOSTFS_RELEASE_BASENAME ${Boost_FILESYSTEM_LIBRARY_RELEASE} NAME_WE)
    get_filename_component(BOOSTFS_RELEASE_PATH ${Boost_FILESYSTEM_LIBRARY_RELEASE} PATH)
    file(COPY "${BOOSTFS_RELEASE_PATH}/${BOOSTFS_RELEASE_BASENAME}.dll" DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE})

	add_custom_command(TARGET ${target}
		POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${CMAKE_SOURCE_DIR}/third_party/vtslog/lib/windows/$<CONFIG>/libcurl.dll" $<TARGET_FILE_DIR:${target}>
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${CMAKE_SOURCE_DIR}/third_party/vtslog/lib/windows/$<CONFIG>/vtslog.dll" $<TARGET_FILE_DIR:${target}>
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${CMAKE_SOURCE_DIR}/third_party/Openssl-Win64/bin/libcrypto-1_1-x64.dll" $<TARGET_FILE_DIR:${target}>
	)
endif()
