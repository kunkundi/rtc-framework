target("rtc_dog_command_forwarder")
    set_kind("static")
    set_default(false)
    add_deps(
        "rtc_vehicle_control_protocol",
        "rtc_vehicle_control_module",
        "rtc_logging"
    )
    add_files(
        vtsrtc_path("client", "modules", "dog", "src",
                    "dog_command_forwarder.cpp")
    )
    add_includedirs(
        vtsrtc_path("client", "modules", "dog", "include"),
        {public = true}
    )
    add_packages("nlohmann_json")
    vtsrtc_add_simple_web_config()
    vtsrtc_add_openssl_config()
    if vtsrtc_on_windows() then
        add_cxflags("/utf-8", {force = true})
    end

target("rtc_dog_command_forwarder_tests")
    set_kind("binary")
    set_default(false)
    add_deps("rtc_dog_command_forwarder")
    add_files(
        vtsrtc_path("client", "modules", "dog", "tests",
                    "dog_command_forwarder_tests.cpp")
    )
    add_includedirs(
        vtsrtc_path("client", "modules", "dog", "include"),
        vtsrtc_path("client", "modules", "vehicle", "include"),
        vtsrtc_path("client", "protocols", "vehicle", "include")
    )
    add_packages("nlohmann_json")
    vtsrtc_add_simple_web_config()
    vtsrtc_add_openssl_config()
