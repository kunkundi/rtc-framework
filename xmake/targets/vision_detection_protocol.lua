target("rtc_vision_detection_protocol")
    set_kind("static")

    add_files(
        vtsrtc_path("protocol", "vision", "generated", "vision_detection.pb.c"),
        vtsrtc_path("protocol", "vision", "src", "vision_detection_codec.cpp")
    )
    add_includedirs(
        vtsrtc_path("protocol", "vision", "include"),
        {public = true}
    )
    add_includedirs(vtsrtc_path("protocol", "vision", "generated"))
    add_packages("nanopb")
    if vtsrtc_on_windows() then
        add_cxflags("/utf-8", {force = true})
    end
