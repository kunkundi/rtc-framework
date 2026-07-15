target("rtc_logging")
    set_kind("static")
    add_files(
        vtsrtc_path("client", "modules", "logging", "src", "rtc_logging.cpp")
    )
    add_includedirs(
        vtsrtc_path("client", "modules", "logging", "include"),
        {public = true}
    )
    add_packages("spdlog", {public = true})
    add_installfiles(
        vtsrtc_path("client", "modules", "logging", "include", "rtc_logging", "rtc_logging.h"),
        {prefixdir = "vtsrtc/include/rtc_logging"}
    )

if vtsrtc_on_linux() then
    target("rtc_logging_tests")
        set_kind("binary")
        set_default(false)
        add_deps("rtc_logging")
        add_files(
            vtsrtc_path("client", "modules", "logging", "tests", "rtc_logging_tests.cpp")
        )
        add_syslinks("pthread")
end
