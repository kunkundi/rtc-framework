target("rtc_vision_detection_protocol")
    set_kind("static")

    add_files(
        vtsrtc_path("client", "protocols", "vision", "generated", "vision_detection.pb.c"),
        vtsrtc_path("client", "protocols", "vision", "src", "vision_detection_codec.cpp"),
        vtsrtc_path("client", "protocols", "vision", "src", "view_control_protocol.cpp")
    )
    add_includedirs(
        vtsrtc_path("client", "protocols", "vision", "include"),
        {public = true}
    )
    add_includedirs(vtsrtc_path("client", "protocols", "vision", "generated"))
    add_packages("nanopb")
    if vtsrtc_on_windows() then
        add_cxflags("/utf-8", {force = true})
    end

target("rtc_vision_view_control_protocol_tests")
    set_kind("binary")
    set_default(false)
    add_deps("rtc_vision_detection_protocol")
    add_files(
        vtsrtc_path("client", "protocols", "vision", "tests", "view_control_protocol_tests.cpp")
    )
    add_includedirs(vtsrtc_path("client", "protocols", "vision", "include"))
    if vtsrtc_on_windows() then
        add_cxflags("/utf-8", {force = true})
    end
