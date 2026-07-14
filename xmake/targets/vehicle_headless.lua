target("rtc_vehicle_control_module_tests")
    set_kind("binary")
    set_default(false)
    add_deps("rtc_vehicle_control_protocol")
    add_files(
        vtsrtc_path("client", "modules", "vehicle", "src", "industrial_control_interface.cpp"),
        vtsrtc_path("client", "modules", "vehicle", "src", "vehicle_control_module.cpp"),
        vtsrtc_path("client", "modules", "vehicle", "tests", "vehicle_control_module_tests.cpp")
    )
    add_includedirs(
        vtsrtc_path("client", "modules", "vehicle", "include"),
        vtsrtc_path("protocol", "control", "include"),
        vtsrtc_path("vtsrtc", "src")
    )
    add_packages("nanopb")
    if vtsrtc_on_windows() then
        add_cxflags("/utf-8", {force = true})
    end

if vtsrtc_on_linux() then
    target("rtc_vehicle_headless")
        set_kind("binary")
        vtsrtc_add_client_dependency()
        add_deps(
            "rtc_dual_camera_image_source",
            "rtc_vehicle_control_protocol"
        )

        add_files(
            vtsrtc_path("client", "apps", "rtc_vehicle_headless", "main.cpp"),
            vtsrtc_path("client", "modules", "vehicle", "src", "vehicle_camera_module.cpp"),
            vtsrtc_path("client", "modules", "vehicle", "src", "industrial_control_interface.cpp"),
            vtsrtc_path("client", "modules", "vehicle", "src", "vehicle_control_module.cpp"),
            vtsrtc_path("client", "modules", "dual_camera", "src", "dual_uyvy_frame_converter.cpp"),
            vtsrtc_path("client", "modules", "dual_camera", "src", "internal", "dual_uyvy_to_i420_stitch_cuda.cu"),
            vtsrtc_path("client", "modules", "headless", "src", "rtc_headless_session.cpp")
        )

        add_includedirs(
            vtsrtc_path("client", "modules", "vehicle", "include"),
            vtsrtc_path("client", "modules", "headless", "include"),
            vtsrtc_path("client", "modules", "dual_camera", "include"),
            vtsrtc_path("client", "modules", "dual_camera", "src", "internal"),
            vtsrtc_path("protocol", "control", "include")
        )
        add_packages("nanopb")
        add_syslinks("pthread")
        add_cuflags("--std=c++14")
        vtsrtc_add_linux_runtime_rpath()

        if not vtsrtc_add_cuda_runtime_config() then
            vtsrtc_fail("CUDA runtime not found for rtc_vehicle_headless")
            set_enabled(false)
        end

        after_buildcmd(function(target, batchcmds)
            batchcmds:cp(path.join(os.projectdir(), "test_data", "rtc.cfg"), target:targetdir())
        end)
end
