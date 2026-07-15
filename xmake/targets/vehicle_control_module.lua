target("rtc_vehicle_control_module")
    set_kind("static")
    set_default(false)
    add_deps("rtc_vehicle_control_protocol")
    add_files(
        vtsrtc_path("client", "modules", "vehicle", "src", "vehicle_control_interface.cpp"),
        vtsrtc_path("client", "modules", "vehicle", "src", "vehicle_control_module.cpp")
    )
    add_includedirs(
        vtsrtc_path("client", "modules", "vehicle", "include"),
        vtsrtc_path("vtsrtc", "src"),
        {public = true}
    )
    if vtsrtc_on_windows() then
        add_cxflags("/utf-8", {force = true})
    end

target("rtc_vehicle_control_module_tests")
    set_kind("binary")
    set_default(false)
    add_deps("rtc_vehicle_control_module")
    add_files(vtsrtc_path("client", "modules", "vehicle", "tests", "vehicle_control_module_tests.cpp"))
    if vtsrtc_on_windows() then
        add_cxflags("/utf-8", {force = true})
    end
