if vtsrtc_on_linux() then
    target("rtc_dual_camera_image_source")
        set_kind("static")
        add_deps("rtc_camera", "rtc_logging")

        add_files(
            vtsrtc_path("client", "modules", "dual_camera", "src", "dual_camera_async_image_source.cpp"),
            vtsrtc_path("client", "modules", "dual_camera", "src", "internal", "async_dual_camera_video_source.cpp")
        )

        add_includedirs(
            vtsrtc_path("client", "modules", "dual_camera", "include"),
            {public = true}
        )
        add_includedirs(
            vtsrtc_path("client", "modules", "dual_camera", "src", "internal")
        )
        add_syslinks("pthread")

        add_installfiles(
            vtsrtc_path("client", "modules", "dual_camera", "include", "rtc_dual_camera", "dual_camera_async_image_source.h"),
            {prefixdir = "vtsrtc/include/rtc_dual_camera"}
        )
end
