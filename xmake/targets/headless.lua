if vtsrtc_on_linux() then
    target("rtc_camera_headless")
        set_kind("binary")
        vtsrtc_add_client_dependency()

        if get_config("enable_miivii_sdk") then
            local miivii_source = vtsrtc_path("client", "modules", "headless", "src", "mv_gmsl_camera.cpp")
            local miivii_header = vtsrtc_path("client", "modules", "headless", "include", "rtc_headless", "mv_gmsl_camera.h")
            if not os.isfile(miivii_source) or not os.isfile(miivii_header) then
                vtsrtc_fail("MiiVii adapter not found under client/modules/headless")
                set_enabled(false)
            else
                add_defines("VTSRTC_USE_MIIVII_SDK")
                add_files(
                    vtsrtc_path("client", "apps", "rtc_camera_headless", "main.cpp"),
                    vtsrtc_path("client", "apps", "rtc_camera_headless", "internal", "uyvy_to_i420_cuda.cu"),
                    vtsrtc_path("client", "modules", "headless", "src", "rtc_camera_common.cpp"),
                    vtsrtc_path("client", "modules", "headless", "src", "rtc_headless_session.cpp"),
                    miivii_source
                )
            end
        else
            add_files(
                vtsrtc_path("client", "apps", "rtc_camera_headless", "main.cpp"),
                vtsrtc_path("client", "apps", "rtc_camera_headless", "internal", "uyvy_to_i420_cuda.cu"),
                vtsrtc_path("client", "modules", "headless", "src", "rtc_camera_common.cpp"),
                vtsrtc_path("client", "modules", "headless", "src", "rtc_headless_session.cpp"),
                vtsrtc_path("client", "modules", "headless", "src", "uyvy_v4l2_camera.cpp")
            )
        end

        add_includedirs(
            vtsrtc_path("client", "apps", "rtc_camera_headless", "internal"),
            vtsrtc_path("client", "modules", "headless", "include")
        )
        if get_config("enable_miivii_sdk") then
            add_includedirs("/opt/miivii/include")
            add_linkdirs("/opt/miivii/lib")
            add_links("mvgmslcamera_noopencv")
        end
        vtsrtc_add_linux_runtime_rpath()
        add_syslinks("pthread")
        add_cuflags("--std=c++14")

        if not vtsrtc_add_cuda_runtime_config() then
            vtsrtc_fail("CUDA runtime not found for rtc_camera_headless")
            set_enabled(false)
        end

        after_buildcmd(function(target, batchcmds)
            batchcmds:cp(path.join(os.projectdir(), "test_data", "rtc.cfg"), target:targetdir())
        end)

    target("rtc_dual_camera_image_source")
        set_kind("static")

        add_files(
            vtsrtc_path("client", "modules", "dual_camera", "src", "dual_camera_async_image_source.cpp"),
            vtsrtc_path("client", "modules", "dual_camera", "src", "internal", "async_dual_camera_video_source.cpp"),
            vtsrtc_path("client", "modules", "headless", "src", "rtc_camera_common.cpp"),
            vtsrtc_path("client", "modules", "headless", "src", "uyvy_v4l2_camera.cpp")
        )

        add_includedirs(
            vtsrtc_path("client", "modules", "dual_camera", "include"),
            {public = true}
        )
        add_includedirs(
            vtsrtc_path("client", "modules", "dual_camera", "src", "internal"),
            vtsrtc_path("client", "modules", "headless", "include")
        )
        add_syslinks("pthread")

        add_installfiles(
            vtsrtc_path("client", "modules", "dual_camera", "include", "rtc_dual_camera", "dual_camera_async_image_source.h"),
            {prefixdir = "vtsrtc/include/rtc_dual_camera"}
        )

    target("rtc_dual_camera_headless")
        set_kind("binary")
        vtsrtc_add_client_dependency()
        add_deps(
            "rtc_dual_camera_image_source",
            "rtc_vision_detection_protocol"
        )

        add_files(
            vtsrtc_path("client", "apps", "rtc_dual_camera_headless", "main.cpp"),
            vtsrtc_path("client", "modules", "dual_camera", "src", "dual_uyvy_frame_converter.cpp"),
            vtsrtc_path("client", "modules", "vision", "src", "stereo_detection_fuser.cpp"),
            vtsrtc_path("client", "modules", "vision", "src", "yolo_frame_consumer.cpp"),
            vtsrtc_path("client", "modules", "vision", "src", "vision_detection_sender.cpp"),
            vtsrtc_path("client", "modules", "dual_camera", "src", "internal", "dual_uyvy_to_i420_stitch_cuda.cu"),
            vtsrtc_path("client", "modules", "headless", "src", "rtc_headless_session.cpp")
        )

        add_includedirs(
            vtsrtc_path("client", "modules", "headless", "include"),
            vtsrtc_path("client", "modules", "dual_camera", "include"),
            vtsrtc_path("client", "modules", "dual_camera", "src", "internal"),
            vtsrtc_path("client", "modules", "vision", "include"),
            vtsrtc_path("protocol", "vision", "include")
        )
        add_packages("nanopb")
        vtsrtc_add_json_config()
        vtsrtc_add_linux_runtime_rpath()
        add_syslinks("pthread")
        add_cuflags("--std=c++14")
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
            vtsrtc_fail("CUDA runtime not found for rtc_dual_camera_headless")
            set_enabled(false)
        end

        after_buildcmd(function(target, batchcmds)
            batchcmds:cp(path.join(os.projectdir(), "test_data", "rtc.cfg"), target:targetdir())
            if get_config("enable_yolo") then
                local model_path = path.join(os.projectdir(), "models", "yolo26n.onnx")
                if os.isfile(model_path) then
                    local model_dir = path.join(target:targetdir(), "models")
                    batchcmds:mkdir(model_dir)
                    batchcmds:cp(model_path, model_dir)
                end
            end
        end)

    target("rtc_receiver_headless")
        set_kind("binary")
        vtsrtc_add_client_dependency()

        add_files(
            vtsrtc_path("client", "apps", "rtc_receiver_headless", "main.cpp"),
            vtsrtc_path("client", "modules", "headless", "src", "rtc_camera_common.cpp"),
            vtsrtc_path("client", "modules", "headless", "src", "rtc_headless_session.cpp")
        )

        add_includedirs(
            vtsrtc_path("client", "modules", "headless", "include")
        )
        vtsrtc_add_linux_runtime_rpath()
        add_syslinks("pthread")

        after_buildcmd(function(target, batchcmds)
            batchcmds:cp(path.join(os.projectdir(), "test_data", "rtc.cfg"), target:targetdir())
        end)
end
