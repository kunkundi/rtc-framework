if vtsrtc_on_linux() then
    target("rtc_runtime")
        set_kind("static")
        vtsrtc_add_client_dependency()
        add_deps("rtc_logging")
        add_files(
            vtsrtc_path("client", "modules", "runtime", "src", "process_runtime.cpp"),
            vtsrtc_path("client", "modules", "runtime", "src", "rtc_session.cpp")
        )
        add_includedirs(
            vtsrtc_path("client", "modules", "runtime", "include"),
            {public = true}
        )
        add_syslinks("pthread", {public = true})
        add_installfiles(
            vtsrtc_path("client", "modules", "runtime", "include", "rtc_runtime", "*.h"),
            {prefixdir = "vtsrtc/include/rtc_runtime"}
        )
end
