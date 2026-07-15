if vtsrtc_on_linux() then
    target("rtc_edge_headless")
        set_kind("binary")
        vtsrtc_add_client_dependency()
        add_deps(
            "rtc_camera",
            "rtc_camera_converter",
            "rtc_headless_common",
            "rtc_logging",
            "rtc_dual_camera_image_source",
            "rtc_vehicle_control_module",
            "rtc_vehicle_control_protocol",
            "rtc_vision_detection_protocol"
        )

        add_files(
            vtsrtc_path("client", "apps", "rtc_edge_headless", "main.cpp"),
            vtsrtc_path("client", "apps", "rtc_edge_headless", "edge_options.cpp"),
            vtsrtc_path("client", "apps", "rtc_edge_headless", "edge_application.cpp"),
            vtsrtc_path("client", "modules", "edge", "src", "dual_camera_streaming_module.cpp"),
            vtsrtc_path("client", "modules", "edge", "src", "single_camera_streaming_module.cpp"),
            vtsrtc_path("client", "modules", "dual_camera", "src", "dual_uyvy_frame_converter.cpp"),
            vtsrtc_path("client", "modules", "vision", "src", "stereo_detection_fuser.cpp"),
            vtsrtc_path("client", "modules", "vision", "src", "yolo_frame_consumer.cpp"),
            vtsrtc_path("client", "modules", "vision", "src", "vision_detection_sender.cpp"),
            vtsrtc_path("client", "modules", "dual_camera", "src", "internal", "dual_uyvy_to_i420_stitch_cuda.cu"),
            vtsrtc_path("client", "modules", "headless", "src", "rtc_headless_session.cpp")
        )

        add_includedirs(
            vtsrtc_path("client", "modules", "edge", "include"),
            vtsrtc_path("client", "modules", "vehicle", "include"),
            vtsrtc_path("client", "modules", "headless", "include"),
            vtsrtc_path("client", "modules", "dual_camera", "include"),
            vtsrtc_path("client", "modules", "dual_camera", "src", "internal"),
            vtsrtc_path("client", "modules", "vision", "include"),
            vtsrtc_path("protocol", "vehicle", "include"),
            vtsrtc_path("protocol", "vision", "include")
        )
        add_packages("nanopb")
        vtsrtc_add_json_config()
        add_syslinks("pthread")
        add_cuflags("--std=c++14")
        vtsrtc_add_linux_runtime_rpath()
        add_includedirs("/usr/include/opencv4")
        add_linkdirs("/usr/local/lib")
        add_links(
            "opencv_calib3d",
            "opencv_features2d",
            "opencv_core"
        )
        add_rpathdirs("/usr/local/lib")

        if get_config("enable_yolo") then
            add_defines("VTSRTC_ENABLE_YOLO_TENSORRT")
            add_files(vtsrtc_path("client", "modules", "vision", "src", "yolo_onnx_detector.cu"))
            add_includedirs("/usr/include/aarch64-linux-gnu")
            add_linkdirs("/lib/aarch64-linux-gnu")
            add_links(
                "nvinfer",
                "nvinfer_plugin",
                "nvonnxparser"
            )
            add_rpathdirs("/lib/aarch64-linux-gnu")
        end

        if not vtsrtc_add_cuda_runtime_config() then
            vtsrtc_fail("CUDA runtime not found for rtc_edge_headless")
            set_enabled(false)
        end

        after_buildcmd(function(target, batchcmds)
            batchcmds:cp(path.join(os.projectdir(), "config", "rtc.cfg"), target:targetdir())
            if get_config("enable_yolo") then
                local model_path = path.join(os.projectdir(), "models", "yolo26n.onnx")
                if os.isfile(model_path) then
                    local model_dir = path.join(target:targetdir(), "models")
                    batchcmds:mkdir(model_dir)
                    batchcmds:cp(model_path, model_dir)
                end
            end
        end)

    target("rtc_edge_options_tests")
        set_kind("binary")
        set_default(false)
        add_deps("rtc_headless_common")
        add_files(
            vtsrtc_path("client", "apps", "rtc_edge_headless", "edge_options.cpp"),
            vtsrtc_path("client", "apps", "rtc_edge_headless", "edge_options_tests.cpp")
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
