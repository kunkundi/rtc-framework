if vtsrtc_on_linux() then
    target("rtc_camera_headless")
        set_kind("binary")
        add_deps(vtsrtc_target_name())

        add_files(
            vtsrtc_path("vtsrtc", "rtc_camera_headless", "app", "main.cpp"),
            vtsrtc_path("vtsrtc", "rtc_camera_headless", "src", "internal", "uyvy_to_i420_cuda.cu"),
            vtsrtc_path("vtsrtc", "rtc_headless_common", "rtc_camera_common.cpp"),
            vtsrtc_path("vtsrtc", "rtc_headless_common", "rtc_headless_session.cpp"),
            vtsrtc_path("vtsrtc", "rtc_headless_common", "uyvy_v4l2_camera.cpp")
        )

        add_includedirs(
            vtsrtc_path("vtsrtc", "src"),
            vtsrtc_path("vtsrtc", "rtc_camera_headless", "src", "internal"),
            vtsrtc_path("vtsrtc", "rtc_headless_common")
        )
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
        set_policy("build.cuda.devlink", true)

        add_files(
            vtsrtc_path("vtsrtc", "rtc_dual_camera_headless", "src", "dual_camera_async_image_source.cpp"),
            vtsrtc_path("vtsrtc", "rtc_dual_camera_headless", "src", "internal", "async_dual_camera_video_source.cpp"),
            vtsrtc_path("vtsrtc", "rtc_dual_camera_headless", "src", "internal", "dual_uyvy_to_i420_stitch_cuda.cu"),
            vtsrtc_path("vtsrtc", "rtc_headless_common", "rtc_camera_common.cpp"),
            vtsrtc_path("vtsrtc", "rtc_headless_common", "uyvy_v4l2_camera.cpp")
        )

        add_includedirs(
            vtsrtc_path("vtsrtc", "rtc_dual_camera_headless", "include"),
            {public = true}
        )
        add_includedirs(
            vtsrtc_path("vtsrtc", "rtc_dual_camera_headless", "src", "internal"),
            vtsrtc_path("vtsrtc", "rtc_headless_common"),
            vtsrtc_path("vtsrtc", "src")
        )
        add_syslinks("pthread")
        add_cuflags("--std=c++14")

        if not vtsrtc_add_cuda_runtime_config() then
            vtsrtc_fail("CUDA runtime not found for rtc_dual_camera_image_source")
            set_enabled(false)
        end

        add_installfiles(
            vtsrtc_path("vtsrtc", "rtc_dual_camera_headless", "include", "rtc_dual_camera", "dual_camera_async_image_source.h"),
            {prefixdir = "vtsrtc/include/rtc_dual_camera"}
        )

    target("rtc_dual_camera_headless")
        set_kind("binary")
        add_deps(vtsrtc_target_name(), "rtc_dual_camera_image_source")

        add_files(
            vtsrtc_path("vtsrtc", "rtc_dual_camera_headless", "app", "main.cpp"),
            vtsrtc_path("vtsrtc", "rtc_dual_camera_headless", "consumers", "yolo_frame_consumer.cpp"),
            vtsrtc_path("vtsrtc", "rtc_dual_camera_headless", "proto", "generated", "vision_detection.pb.c"),
            vtsrtc_path("vtsrtc", "rtc_dual_camera_headless", "proto", "vision_detection_codec.cpp"),
            vtsrtc_path("vtsrtc", "rtc_dual_camera_headless", "proto", "vision_detection_sender.cpp"),
            vtsrtc_path("vtsrtc", "rtc_headless_common", "rtc_headless_session.cpp")
        )

        add_includedirs(
            vtsrtc_path("vtsrtc", "src"),
            vtsrtc_path("vtsrtc", "rtc_headless_common"),
            vtsrtc_path("vtsrtc", "rtc_dual_camera_headless", "include"),
            vtsrtc_path("vtsrtc", "rtc_dual_camera_headless", "consumers"),
            vtsrtc_path("vtsrtc", "rtc_dual_camera_headless", "proto"),
            vtsrtc_path("vtsrtc", "rtc_dual_camera_headless", "proto", "generated")
        )
        add_packages("nanopb")
        vtsrtc_add_linux_runtime_rpath()
        add_syslinks("pthread")

        if not vtsrtc_add_cuda_runtime_config() then
            vtsrtc_fail("CUDA runtime not found for rtc_dual_camera_headless")
            set_enabled(false)
        end

        after_buildcmd(function(target, batchcmds)
            batchcmds:cp(path.join(os.projectdir(), "test_data", "rtc.cfg"), target:targetdir())
        end)

    target("rtc_receiver_headless")
        set_kind("binary")
        add_deps(vtsrtc_target_name())

        add_files(
            vtsrtc_path("vtsrtc", "rtc_receiver_headless", "app", "main.cpp"),
            vtsrtc_path("vtsrtc", "rtc_headless_common", "rtc_camera_common.cpp"),
            vtsrtc_path("vtsrtc", "rtc_headless_common", "rtc_headless_session.cpp")
        )

        add_includedirs(
            vtsrtc_path("vtsrtc", "src"),
            vtsrtc_path("vtsrtc", "rtc_headless_common")
        )
        vtsrtc_add_linux_runtime_rpath()
        add_syslinks("pthread")

        after_buildcmd(function(target, batchcmds)
            batchcmds:cp(path.join(os.projectdir(), "test_data", "rtc.cfg"), target:targetdir())
        end)
end
