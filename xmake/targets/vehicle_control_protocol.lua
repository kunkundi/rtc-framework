target("rtc_vehicle_control_protocol")
    set_kind("static")

    add_files(
        vtsrtc_path("protocol", "control", "generated", "rtc_control.pb.c"),
        vtsrtc_path("protocol", "control", "src", "vehicle_control_protocol.cpp")
    )
    add_includedirs(
        vtsrtc_path("protocol", "control", "include"),
        {public = true}
    )
    add_includedirs(vtsrtc_path("protocol", "control", "generated"))
    add_packages("nanopb")
    if vtsrtc_on_windows() then
        add_cxflags("/utf-8", {force = true})
    end

    add_installfiles(
        vtsrtc_path("protocol", "control", "include", "rtc_control", "vehicle_control_protocol.h"),
        {prefixdir = "rtc_vehicle_control_protocol/include/rtc_control"}
    )

target("rtc_vehicle_control_protocol_tests")
    set_kind("binary")
    set_default(false)
    add_deps("rtc_vehicle_control_protocol")
    add_files(vtsrtc_path("protocol", "control", "tests", "vehicle_control_protocol_tests.cpp"))
    add_includedirs(
        vtsrtc_path("protocol", "control", "include"),
        vtsrtc_path("protocol", "control", "generated")
    )
    add_packages("nanopb")
    if vtsrtc_on_windows() then
        add_cxflags("/utf-8", {force = true})
    end
