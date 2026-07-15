target("p2p_imgui")
    set_kind("binary")
    vtsrtc_add_client_dependency()
    add_deps("rtc_vision_detection_protocol")
    add_packages("imgui")

    add_files(vtsrtc_path("client", "apps", "p2p_imgui", "main.cpp"))
    add_packages("nanopb")
    vtsrtc_add_linux_runtime_rpath()
    if vtsrtc_on_windows() then
        add_syslinks("opengl32")
    elseif vtsrtc_on_linux() then
        add_syslinks("pthread", "GL", "dl", "asound")
    end

    if vtsrtc_on_linux() then
        after_buildcmd(function(target, batchcmds)
            local config_file = path.join(os.projectdir(), "config", "rtc.cfg")
            if os.exists(config_file) then
                batchcmds:cp(config_file, target:targetdir())
            end

            local test_data = path.join(os.projectdir(), "test_data")
            for _, f in ipairs({"8k16bit.pcm", "zjlabs.yuv", "messagefile.txt"}) do
                local src = path.join(test_data, f)
                if os.exists(src) then
                    batchcmds:cp(src, target:targetdir())
                end
            end
        end)
    end
