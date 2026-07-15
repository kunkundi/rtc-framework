if vtsrtc_on_linux() then
    target("rtc_camera")
        set_kind("static")
        add_deps("rtc_logging")
        add_files(
            vtsrtc_path("client", "modules", "camera", "src", "async_camera_image_source.cpp"),
            vtsrtc_path("client", "modules", "camera", "src", "v4l2_camera.cpp")
        )
        add_includedirs(
            vtsrtc_path("client", "modules", "camera", "include"),
            {public = true}
        )
        add_syslinks("pthread", {public = true})
        add_installfiles(
            vtsrtc_path("client", "modules", "camera", "include", "rtc_camera", "*.h"),
            {prefixdir = "vtsrtc/include/rtc_camera"}
        )

    target("rtc_camera_converter")
        set_kind("static")
        set_policy("build.cuda.devlink", true)
        add_deps("rtc_camera")
        add_files(
            vtsrtc_path("client", "modules", "camera", "src", "camera_frame_converter.cpp"),
            vtsrtc_path("client", "modules", "camera", "src", "uyvy_to_i420_cuda.cu")
        )
        add_cuflags("--std=c++14")

        if not vtsrtc_add_cuda_runtime_config() then
            vtsrtc_fail("CUDA runtime not found for rtc_camera_converter")
            set_enabled(false)
        end

    target("rtc_camera_tests")
        set_kind("binary")
        set_default(false)
        add_deps("rtc_camera", "rtc_camera_converter")
        add_files(
            vtsrtc_path("client", "modules", "camera", "tests", "rtc_camera_tests.cpp")
        )
        add_syslinks("pthread")
        add_cuflags("--std=c++14")

        if not vtsrtc_add_cuda_runtime_config() then
            vtsrtc_fail("CUDA runtime not found for rtc_camera_tests")
            set_enabled(false)
        end
end
