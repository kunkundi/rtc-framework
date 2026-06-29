target("p2p_imgui")
    set_kind("binary")
    vtsrtc_add_client_dependency()
    add_packages("imgui")

    add_files(
        vtsrtc_path("client", "p2p_imgui", "main.cpp"),
        vtsrtc_path("client", "rtc_dual_camera_headless", "proto", "generated", "vision_detection.pb.c"),
        vtsrtc_path("client", "rtc_dual_camera_headless", "proto", "vision_detection_codec.cpp")
    )

    add_includedirs(
        vtsrtc_path("client", "rtc_dual_camera_headless", "proto"),
        vtsrtc_path("client", "rtc_dual_camera_headless", "proto", "generated")
    )
    add_packages("nanopb")
    vtsrtc_add_linux_runtime_rpath()
    if vtsrtc_on_windows() then
        add_syslinks("opengl32")
    elseif vtsrtc_on_linux() then
        add_syslinks("pthread", "GL", "dl", "asound")
    end

    if vtsrtc_on_linux() then
        after_buildcmd(function(target, batchcmds)
            batchcmds:cp(path.join(os.projectdir(), "test_data", "rtc.cfg"), target:targetdir())
            batchcmds:cp(path.join(os.projectdir(), "test_data", "8k16bit.pcm"), target:targetdir())
            batchcmds:cp(path.join(os.projectdir(), "test_data", "zjlabs.yuv"), target:targetdir())
            batchcmds:cp(path.join(os.projectdir(), "test_data", "messagefile.txt"), target:targetdir())
        end)
    end
