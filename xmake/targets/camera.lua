if vtsrtc_on_linux() then
    target("rtc_camera")
        set_kind("static")
        add_deps("rtc_logging")
        add_files(
            vtsrtc_path("client", "modules", "camera", "src", "v4l2_camera.cpp"),
            vtsrtc_path("client", "modules", "camera", "src", "single", "async_image_source.cpp"),
            vtsrtc_path("client", "modules", "camera", "src", "dual", "async_image_source.cpp"),
            vtsrtc_path("client", "modules", "camera", "src", "dual", "internal", "async_video_source.cpp")
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
        add_installfiles(
            vtsrtc_path("client", "modules", "camera", "include", "rtc_camera", "single", "*.h"),
            {prefixdir = "vtsrtc/include/rtc_camera/single"}
        )
        add_installfiles(
            vtsrtc_path("client", "modules", "camera", "include", "rtc_camera", "dual", "*.h"),
            {prefixdir = "vtsrtc/include/rtc_camera/dual"}
        )

    target("rtc_camera_converter")
        set_kind("static")
        set_policy("build.cuda.devlink", true)
        add_deps("rtc_camera")
        add_files(
            vtsrtc_path("client", "modules", "camera", "src", "single", "frame_converter.cpp"),
            vtsrtc_path("client", "modules", "camera", "src", "single", "uyvy_to_i420_cuda.cu"),
            vtsrtc_path("client", "modules", "camera", "src", "dual", "frame_converter.cpp"),
            vtsrtc_path("client", "modules", "camera", "src", "dual", "internal", "uyvy_to_i420_stitch_cuda.cu")
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
            vtsrtc_path("client", "modules", "camera", "tests", "single", "rtc_camera_tests.cpp")
        )
        add_syslinks("pthread")
        add_cuflags("--std=c++14")

        if not vtsrtc_add_cuda_runtime_config() then
            vtsrtc_fail("CUDA runtime not found for rtc_camera_tests")
            set_enabled(false)
        end

    target("rtc_camera_dual_tests")
        set_kind("binary")
        set_default(false)
        add_deps("rtc_camera", "rtc_camera_converter")
        add_files(
            vtsrtc_path("client", "modules", "camera", "tests", "dual", "rtc_dual_camera_tests.cpp")
        )
        add_syslinks("pthread")
        add_cuflags("--std=c++14")

        if not vtsrtc_add_cuda_runtime_config() then
            vtsrtc_fail("CUDA runtime not found for rtc_camera_dual_tests")
            set_enabled(false)
        end
end
