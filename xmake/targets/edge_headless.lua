if vtsrtc_on_linux() then
    target("rtc_edge_headless")
        set_kind("binary")
        vtsrtc_add_client_dependency()
        add_deps(
            "rtc_dual_camera_image_source",
            "rtc_vehicle_control_module",
            "rtc_vehicle_control_protocol"
        )

        add_files(
            vtsrtc_path("client", "apps", "rtc_edge_headless", "main.cpp"),
            vtsrtc_path("client", "apps", "rtc_edge_headless", "edge_options.cpp"),
            vtsrtc_path("client", "apps", "rtc_edge_headless", "edge_application.cpp"),
            vtsrtc_path("client", "modules", "edge", "src", "dual_camera_streaming_module.cpp"),
            vtsrtc_path("client", "modules", "dual_camera", "src", "dual_uyvy_frame_converter.cpp"),
            vtsrtc_path("client", "modules", "dual_camera", "src", "internal", "dual_uyvy_to_i420_stitch_cuda.cu"),
            vtsrtc_path("client", "modules", "headless", "src", "rtc_headless_session.cpp")
        )

        add_includedirs(
            vtsrtc_path("client", "modules", "edge", "include"),
            vtsrtc_path("client", "modules", "vehicle", "include"),
            vtsrtc_path("client", "modules", "headless", "include"),
            vtsrtc_path("client", "modules", "dual_camera", "include"),
            vtsrtc_path("client", "modules", "dual_camera", "src", "internal"),
            vtsrtc_path("protocol", "vehicle", "include")
        )
        add_packages("nanopb")
        vtsrtc_add_json_config()
        add_syslinks("pthread")
        add_cuflags("--std=c++14")
        vtsrtc_add_linux_runtime_rpath()

        if not vtsrtc_add_cuda_runtime_config() then
            vtsrtc_fail("CUDA runtime not found for rtc_edge_headless")
            set_enabled(false)
        end

        after_buildcmd(function(target, batchcmds)
            batchcmds:cp(path.join(os.projectdir(), "test_data", "rtc.cfg"), target:targetdir())
        end)

    target("rtc_edge_options_tests")
        set_kind("binary")
        set_default(false)
        add_files(
            vtsrtc_path("client", "apps", "rtc_edge_headless", "edge_options.cpp"),
            vtsrtc_path("client", "apps", "rtc_edge_headless", "edge_options_tests.cpp"),
            vtsrtc_path("client", "modules", "headless", "src", "rtc_camera_common.cpp")
        )
        add_includedirs(
            vtsrtc_path("client", "apps", "rtc_edge_headless"),
            vtsrtc_path("client", "modules", "edge", "include"),
            vtsrtc_path("client", "modules", "vehicle", "include"),
            vtsrtc_path("client", "modules", "headless", "include"),
            vtsrtc_path("client", "modules", "dual_camera", "include"),
            vtsrtc_path("protocol", "vehicle", "include"),
            vtsrtc_path("vtsrtc", "src")
        )
        vtsrtc_add_json_config()
        add_syslinks("pthread")
end
