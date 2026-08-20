target("rtc_audio")
    set_kind("static")
    add_files(vtsrtc_path("client", "modules", "audio", "src", "audio_device.cpp"))
    add_includedirs(
        vtsrtc_path("client", "modules", "audio", "include"),
        {public = true}
    )
    add_packages("libsdl3", {public = true})
    if vtsrtc_on_linux() then
        add_syslinks("pthread", {public = true})
    elseif vtsrtc_on_windows() then
        add_cxflags("/utf-8", {force = true})
    end
    add_installfiles(
        vtsrtc_path("client", "modules", "audio", "include", "rtc_audio", "*.h"),
        {prefixdir = "vtsrtc/include/rtc_audio"}
    )

target("rtc_audio_device_tests")
    set_kind("binary")
    set_default(false)
    add_deps("rtc_audio")
    add_files(vtsrtc_path("client", "modules", "audio", "tests", "audio_device_tests.cpp"))
    if vtsrtc_on_windows() then
        add_cxflags("/utf-8", {force = true})
    end
