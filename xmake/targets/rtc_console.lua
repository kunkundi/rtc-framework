target("rtc_console")
    set_kind("binary")
    vtsrtc_add_client_dependency()
    add_deps(
        "rtc_vehicle_control_protocol",
        "rtc_vision_detection_protocol"
    )
    add_packages("imgui")

    add_files(
        vtsrtc_path("client", "apps", "rtc_console", "main.cpp"),
        vtsrtc_path("client", "apps", "rtc_console", "rtc_console_options.cpp"),
        vtsrtc_path("client", "apps", "rtc_console", "vehicle_control_sender.cpp"),
        vtsrtc_path("client", "apps", "rtc_console", "vehicle_state_log_limiter.cpp")
    )
    add_includedirs(vtsrtc_path("client", "apps", "rtc_console"))
    add_packages("nanopb")
    vtsrtc_add_json_config()
    vtsrtc_add_linux_runtime_rpath()
    if vtsrtc_on_windows() then
        add_cxflags("/utf-8", {force = true})
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
            for _, f in ipairs({"8k16bit.pcm", "zjlabs.yuv"}) do
                local src = path.join(test_data, f)
                if os.exists(src) then
                    batchcmds:cp(src, target:targetdir())
                end
            end
        end)
    end

target("rtc_console_options_tests")
    set_kind("binary")
    set_default(false)
    add_files(
        vtsrtc_path("client", "apps", "rtc_console", "rtc_console_options.cpp"),
        vtsrtc_path("client", "apps", "rtc_console", "rtc_console_options_tests.cpp")
    )
    add_includedirs(vtsrtc_path("client", "apps", "rtc_console"))
    vtsrtc_add_json_config()
    if vtsrtc_on_windows() then
        add_cxflags("/utf-8", {force = true})
    end

target("rtc_console_vehicle_state_log_tests")
    set_kind("binary")
    set_default(false)
    add_files(
        vtsrtc_path("client", "apps", "rtc_console", "vehicle_state_log_limiter.cpp"),
        vtsrtc_path("client", "apps", "rtc_console", "vehicle_state_log_limiter_tests.cpp")
    )
    add_includedirs(vtsrtc_path("client", "apps", "rtc_console"))
    if vtsrtc_on_windows() then
        add_cxflags("/utf-8", {force = true})
    end

target("rtc_console_vehicle_control_sender_tests")
    set_kind("binary")
    set_default(false)
    add_deps("rtc_vehicle_control_protocol")
    add_files(
        vtsrtc_path("client", "apps", "rtc_console", "vehicle_control_sender.cpp"),
        vtsrtc_path("client", "apps", "rtc_console", "vehicle_control_sender_tests.cpp")
    )
    add_includedirs(vtsrtc_path("client", "apps", "rtc_console"))
    if vtsrtc_on_windows() then
        add_cxflags("/utf-8", {force = true})
    end
