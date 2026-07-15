set_project("rtc-framework")
set_version("0.5.0")

includes("@builtin/check")

add_rules("mode.debug", "mode.release")
set_languages("cxx14")

includes("xmake/platform.lua")
vtsrtc_configure_targetdir()

includes("xmake/options.lua")
includes("xmake/dependencies.lua")
includes("xmake/encoders.lua")
includes("xmake/yolo.lua")

if not vtsrtc_has_prebuilt_lib() then
    includes("xmake/targets/vtsrtc.lua")
end
includes("xmake/targets/signaling_server.lua")
includes("xmake/targets/vehicle_control_protocol.lua")
includes("xmake/targets/vehicle_control_module.lua")
includes("xmake/targets/vision_detection_protocol.lua")
includes("xmake/targets/p2p_imgui.lua")
includes("xmake/targets/headless.lua")
includes("xmake/targets/edge_headless.lua")
