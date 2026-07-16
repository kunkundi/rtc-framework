target("rtc_vehicle_control_protocol")
    set_kind("static")

    add_files(
        vtsrtc_path("client", "protocols", "vehicle", "generated", "rtc_vehicle.pb.c"),
        vtsrtc_path("client", "protocols", "vehicle", "src", "vehicle_control_protocol.cpp")
    )
    add_includedirs(
        vtsrtc_path("client", "protocols", "vehicle", "include"),
        {public = true}
    )
    add_includedirs(vtsrtc_path("client", "protocols", "vehicle", "generated"))
    add_packages("nanopb")
    if vtsrtc_on_windows() then
        add_cxflags("/utf-8", {force = true})
    end

    add_installfiles(
        vtsrtc_path("client", "protocols", "vehicle", "include", "rtc_vehicle_protocol", "vehicle_control_protocol.h"),
        {prefixdir = "rtc_vehicle_control_protocol/include/rtc_vehicle_protocol"}
    )

target("rtc_vehicle_control_protocol_tests")
    set_kind("binary")
    set_default(false)
    add_deps("rtc_vehicle_control_protocol")
    add_files(vtsrtc_path("client", "protocols", "vehicle", "tests", "vehicle_control_protocol_tests.cpp"))
    add_includedirs(
        vtsrtc_path("client", "protocols", "vehicle", "include"),
        vtsrtc_path("client", "protocols", "vehicle", "generated")
    )
    add_packages("nanopb")
    if vtsrtc_on_windows() then
        add_cxflags("/utf-8", {force = true})
    end
