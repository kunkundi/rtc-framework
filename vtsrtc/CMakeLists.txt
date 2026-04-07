add_subdirectory(src)
#enable_testing()
#add_subdirectory(test)
option(BUILD_VTSRTC_EXAMPLES "Build vtsrtc Qt examples" ON)

if(BUILD_VTSRTC_EXAMPLES)
	add_subdirectory(examples)
endif()
