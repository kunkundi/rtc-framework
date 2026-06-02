target("p2p_imgui")
    set_kind("binary")
    add_deps(vtsrtc_target_name())
    add_packages("imgui")

    add_files(
        vtsrtc_path("vtsrtc", "p2p_imgui", "main.cpp")
    )

    add_includedirs(vtsrtc_path("vtsrtc", "src"))
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
