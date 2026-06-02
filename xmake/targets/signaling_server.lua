target("signaling-server")
    set_kind("binary")
    set_languages("cxx17")

    add_files(
        vtsrtc_path("signaling-server", "src", "log_manager.cpp"),
        vtsrtc_path("signaling-server", "src", "http_controller.cpp"),
        vtsrtc_path("signaling-server", "src", "ws_controller.cpp"),
        vtsrtc_path("signaling-server", "src", "main.cpp")
    )

    add_includedirs(vtsrtc_path("signaling-server", "src"))
    vtsrtc_add_simple_web_config()
    vtsrtc_add_json_config()
    vtsrtc_add_spdlog_config()
    vtsrtc_add_openssl_config()
    vtsrtc_add_linux_runtime_rpath()

    if vtsrtc_on_linux() then
        after_buildcmd(function(target, batchcmds)
            batchcmds:cp(path.join(os.projectdir(), "test_data", "web"), path.join(target:targetdir(), "web"))
            batchcmds:cp(path.join(os.projectdir(), "test_data", "signaling-server.cfg"), target:targetdir())
        end)
    end
