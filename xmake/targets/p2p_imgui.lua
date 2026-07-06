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
            local test_data = path.join(os.projectdir(), "test_data")
            for _, f in ipairs({"rtc.cfg", "8k16bit.pcm", "zjlabs.yuv", "messagefile.txt"}) do
                local src = path.join(test_data, f)
                if os.exists(src) then
                    batchcmds:cp(src, target:targetdir())
                end
            end
        end)
    end
