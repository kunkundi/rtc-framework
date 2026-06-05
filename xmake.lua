set_project("rtc-solutions")
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

includes("xmake/targets/vtsrtc.lua")
includes("xmake/targets/signaling_server.lua")
includes("xmake/targets/p2p_imgui.lua")
includes("xmake/targets/headless.lua")
