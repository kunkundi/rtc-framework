target("rtc_yolo_onnx")
    set_kind("static")
    add_deps("rtc_runtime", "rtc_vision_detection_protocol")
    add_packages("onnxruntime", "libyuv", {public = true})
    add_includedirs(vtsrtc_path("client", "modules", "vision", "include"), {public = true})
    add_includedirs(vtsrtc_path("client", "modules", "camera", "include"), {public = true})
    add_files(vtsrtc_path("client", "modules", "vision", "src", "yolo_onnx_detector.cpp"))
    if vtsrtc_on_windows() then
        add_cxflags("/utf-8", {force = true})
    end

target("rtc_console")
    set_kind("binary")
    vtsrtc_add_client_dependency()
    add_deps(
        "rtc_audio",
        "rtc_yolo_onnx",
        "rtc_runtime",
        "rtc_vehicle_control_protocol",
        "rtc_vision_detection_protocol"
    )
    add_packages("imgui")

    add_files(
        vtsrtc_path("client", "apps", "rtc_console", "local_yolo.cpp"),
        vtsrtc_path("client", "apps", "rtc_console", "main.cpp"),
        vtsrtc_path("client", "apps", "rtc_console", "rtc_console_options.cpp"),
        vtsrtc_path("client", "apps", "rtc_console", "video_composite_layout.cpp"),
        vtsrtc_path("client", "apps", "rtc_console", "video_view_state.cpp"),
        vtsrtc_path("client", "apps", "rtc_console", "vehicle_control_sender.cpp"),
        vtsrtc_path("client", "apps", "rtc_console", "vehicle_state_log_limiter.cpp")
    )
    add_includedirs(vtsrtc_path("client", "apps", "rtc_console"))
    add_packages("nanopb")
    vtsrtc_add_json_config()
    vtsrtc_add_linux_runtime_rpath()
    if vtsrtc_on_windows() then
        add_cxflags("/utf-8", {force = true})
        add_syslinks("opengl32", "dwmapi")
    elseif vtsrtc_on_linux() then
        add_syslinks("pthread", "GL", "dl")
    end

    after_build(function(target)
        local runtime = target:dep("rtc_yolo_onnx"):pkg("onnxruntime"):installdir()
        if target:is_plat("windows") then
            os.cp(path.join(runtime, "bin", "*.dll"), target:targetdir())
        else
            os.cp(path.join(runtime, "lib", "*.so*"), target:targetdir())
        end
        local model = path.join(os.projectdir(), "models", "yolo26n.onnx")
        if os.isfile(model) then
            os.mkdir(path.join(target:targetdir(), "models"))
            os.cp(model, path.join(target:targetdir(), "models"))
        end
    end)
    after_install(function(target)
        local destination = path.join(target:installdir(), "bin")
        local runtime = target:dep("rtc_yolo_onnx"):pkg("onnxruntime"):installdir()
        if target:is_plat("windows") then
            os.cp(path.join(runtime, "bin", "*.dll"), destination)
        else
            os.cp(path.join(runtime, "lib", "*.so*"), destination)
        end
        local model = path.join(os.projectdir(), "models", "yolo26n.onnx")
        if os.isfile(model) then
            os.mkdir(path.join(destination, "models"))
            os.cp(model, path.join(destination, "models"))
        end
    end)
    if vtsrtc_on_linux() or vtsrtc_on_windows() then
        after_buildcmd(function(target, batchcmds)
            for _, name in ipairs({"rtc.cfg", "console.cfg", "edge.cfg"}) do
                batchcmds:cp(path.join(os.projectdir(), "config", name), target:targetdir())
            end

            local test_data = path.join(os.projectdir(), "test_data")
            for _, f in ipairs({"zjlabs.yuv"}) do
                local src = path.join(test_data, f)
                if os.exists(src) then
                    batchcmds:cp(src, target:targetdir())
                end
            end
        end)
    end

target("rtc_console_local_yolo_tests")
    set_kind("binary")
    set_default(false)
    add_deps("rtc_yolo_onnx")
    add_files(vtsrtc_path("client", "apps", "rtc_console", "local_yolo.cpp"),
              vtsrtc_path("client", "apps", "rtc_console", "local_yolo_tests.cpp"))
    if vtsrtc_on_windows() then
        add_cxflags("/utf-8", {force = true})
    elseif vtsrtc_on_linux() then
        add_syslinks("pthread")
    end

target("rtc_console_options_tests")
    set_kind("binary")
    set_default(false)
    add_deps("rtc_runtime")
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

target("rtc_console_video_view_state_tests")
    set_kind("binary")
    set_default(false)
    add_deps("rtc_vision_detection_protocol")
    add_files(
        vtsrtc_path("client", "apps", "rtc_console", "video_view_state.cpp"),
        vtsrtc_path("client", "apps", "rtc_console", "video_view_state_tests.cpp")
    )
    add_includedirs(vtsrtc_path("client", "apps", "rtc_console"))
    if vtsrtc_on_windows() then
        add_cxflags("/utf-8", {force = true})
    end

target("rtc_console_video_composite_layout_tests")
    set_kind("binary")
    set_default(false)
    add_files(
        vtsrtc_path("client", "apps", "rtc_console", "video_composite_layout.cpp"),
        vtsrtc_path("client", "apps", "rtc_console", "video_composite_layout_tests.cpp")
    )
    add_includedirs(vtsrtc_path("client", "apps", "rtc_console"))
    if vtsrtc_on_windows() then
        add_cxflags("/utf-8", {force = true})
    end

target("rtc_console_video_fullscreen_state_tests")
    set_kind("binary")
    set_default(false)
    add_files(vtsrtc_path("client", "apps", "rtc_console", "video_fullscreen_state_tests.cpp"))
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
