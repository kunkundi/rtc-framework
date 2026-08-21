local vtsrtc_target = vtsrtc_target_name()

target(vtsrtc_target)
    set_kind("shared")
    if get_config("use_prebuilt_vtsrtc") then
        set_default(false)
    end
    if vtsrtc_is_aarch64_arch() then
        set_basename("vtsrtc.aarch64")
    else
        set_basename("vtsrtc")
    end

    if get_config("enable_encode_perf_stats") then
        add_defines("ENABLE_ENCODE_PERF_STATS=1")
    else
        add_defines("ENABLE_ENCODE_PERF_STATS=0")
    end

    add_includedirs(vtsrtc_path("vtsrtc", "src"), {public = true})
    add_files(
        vtsrtc_path("vtsrtc", "src", "log", "log_manager.cpp"),
        vtsrtc_path("vtsrtc", "src", "log", "log_webrtc_hook.cpp"),
        vtsrtc_path("vtsrtc", "src", "rtc_device_manager.cpp"),
        vtsrtc_path("vtsrtc", "src", "rtc_connection.cpp"),
        vtsrtc_path("vtsrtc", "src", "rtc_connection_manager.cpp"),
        vtsrtc_path("vtsrtc", "src", "rtc.cpp"),
        vtsrtc_path("vtsrtc", "src", "c_rtc.cpp"),
        vtsrtc_path("vtsrtc", "src", "video", "encode", "rtc_encoder_factory.cpp"),
        vtsrtc_path("vtsrtc", "src", "statistics", "rtc_statistics.cpp"),
        vtsrtc_path("vtsrtc", "src", "packet", "pack.cpp"),
        vtsrtc_path("vtsrtc", "src", "packet", "packetizer.cpp"),
        vtsrtc_path("vtsrtc", "src", "packet", "depacketizer.cpp")
    )

    if get_config("use_default_jetson_encoder") then
        add_defines("USE_DEFAULT_JETSON_ENCODER")
    end

    if vtsrtc_on_windows() then
        add_defines("WEBRTC_WIN", "NOMINMAX", "WIN32_LEAN_AND_MEAN", "RTC_DLL_EXPORTS")
        add_syslinks("winmm", "Secur32", "Msdmo", "Dmoguids", "wmcodecdspuuid", "Strmiids", "Advapi32")
    elseif vtsrtc_on_linux() then
        add_defines("WEBRTC_LINUX", "WEBRTC_POSIX")
        add_syslinks("X11", "rt")
    end

    vtsrtc_add_simple_web_config()
    vtsrtc_add_json_config()
    vtsrtc_add_spdlog_config()
    vtsrtc_add_webrtc_config()
    vtsrtc_add_linux_runtime_rpath()
    vtsrtc_add_linux_symbol_visibility(vtsrtc_path("vtsrtc", "vtsrtc.map"))

    if vtsrtc_is_aarch64_arch() then
        if not get_config("use_default_jetson_encoder") then
            vtsrtc_add_jetson_h264_encoder_config()
            vtsrtc_add_rasp_h264_encoder_config()
        end
    else
        add_files(
            vtsrtc_path("vtsrtc", "src", "video", "decode", "rtc_decoder_factory.cpp")
        )

        if vtsrtc_add_cuda_driver_config() then
            add_files(
                vtsrtc_path("vtsrtc", "src", "video", "encode", "nvidia", "nvh264_encoder_impl.cpp"),
                vtsrtc_path("vtsrtc", "src", "video", "decode", "nvidia", "nvh264_decoder_impl.cpp"),
                vtsrtc_path("third_party", "Video_Codec_SDK_11.0.10", "Samples", "NvCodec", "NvEncoder", "NvEncoder.cpp"),
                vtsrtc_path("third_party", "Video_Codec_SDK_11.0.10", "Samples", "NvCodec", "NvEncoder", "NvEncoderCuda.cpp"),
                vtsrtc_path("third_party", "Video_Codec_SDK_11.0.10", "Samples", "NvCodec", "NvDecoder", "NvDecoder.cpp")
            )
            vtsrtc_add_nvcodec_config()
        else
            add_defines("VTSRTC_HAS_CUDA_DRIVER=0")
            if vtsrtc_get_bool_config("enable_cuda", true) then
                print("warning: CUDA toolkit not found, NVENC/NVDEC sources are disabled for this build")
            else
                print("warning: CUDA support disabled by build config, NVENC/NVDEC sources are disabled for this build")
            end
        end
    end

    add_installfiles(vtsrtc_path("vtsrtc", "src", "c_rtc.h"), {prefixdir = "vtsrtc/include"})
    add_installfiles(vtsrtc_path("config", "rtc.cfg"), {prefixdir = "vtsrtc"})

    after_build(function(target)
        local lib_dir = path.join(os.projectdir(), "lib")
        os.mkdir(lib_dir)
        local src_file = target:targetfile()
        os.cp(src_file, lib_dir)
        print("vtsrtc: copied to " .. path.join(lib_dir, path.filename(src_file)))
        if is_plat("windows") then
            local dll_name = path.basename(src_file)
            local implib_src = path.join(target:targetdir(), dll_name .. ".lib")
            if os.isfile(implib_src) then
                os.cp(implib_src, lib_dir)
                print("vtsrtc: copied import library to " .. lib_dir)
            end
        end
    end)

target("rtc_external_audio_device_tests")
    set_kind("binary")
    set_default(false)
    add_files(vtsrtc_path("vtsrtc", "tests", "rtc_external_audio_device_tests.cpp"))
    add_includedirs(vtsrtc_path("vtsrtc", "src"))
    vtsrtc_add_webrtc_config()
    if vtsrtc_on_windows() then
        add_defines("WEBRTC_WIN", "NOMINMAX", "WIN32_LEAN_AND_MEAN")
        add_syslinks("winmm")
    elseif vtsrtc_on_linux() then
        add_defines("WEBRTC_LINUX", "WEBRTC_POSIX")
        add_syslinks("pthread", "rt")
    end

if vtsrtc_is_aarch64_arch() and not get_config("use_default_jetson_encoder") then
    target("rtc_jetson_encoder_switch_benchmark")
        set_kind("binary")
        set_default(false)
        add_files(
            vtsrtc_path("vtsrtc", "tests", "rtc_jetson_encoder_switch_benchmark.cpp"),
            vtsrtc_path("vtsrtc", "src", "log", "log_manager.cpp")
        )
        add_includedirs(vtsrtc_path("vtsrtc", "src"))
        add_defines("WEBRTC_LINUX", "WEBRTC_POSIX")
        add_syslinks("pthread", "rt")
        vtsrtc_add_spdlog_config()
        vtsrtc_add_webrtc_config()
        if not vtsrtc_add_jetson_encoder_backend_config() then
            set_enabled(false)
        end

    target("rtc_jetson_h264_encoder_integration_tests")
        set_kind("binary")
        set_default(false)
        add_files(
            vtsrtc_path("vtsrtc", "tests", "rtc_jetson_h264_encoder_integration_tests.cpp"),
            vtsrtc_path("vtsrtc", "src", "log", "log_manager.cpp"),
            vtsrtc_path("vtsrtc", "src", "video", "encode", "nvidia-jetson", "jetsonh264_encoder_impl.cpp")
        )
        add_includedirs(vtsrtc_path("vtsrtc", "src"))
        add_defines("WEBRTC_LINUX", "WEBRTC_POSIX")
        add_syslinks("pthread", "rt")
        vtsrtc_add_spdlog_config()
        vtsrtc_add_webrtc_config()
        if not vtsrtc_add_jetson_encoder_backend_config() then
            set_enabled(false)
        end
end
