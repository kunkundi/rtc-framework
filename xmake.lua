set_project("rtc-solutions")
set_version("0.5.0")

add_rules("mode.debug", "mode.release")
set_languages("cxx14")

local function on_windows()
    return is_plat("windows") or is_host("windows")
end

local function on_linux()
    return is_plat("linux") or is_host("linux")
end

if on_windows() then
    set_targetdir("$(builddir)/runtime/$(mode)")
else
    set_targetdir("$(builddir)/runtime")
end

option("asio_path")
    set_default("")
    set_showmenu(true)
    set_description("Standalone Asio include directory")
option_end()

option("use_default_jetson_encoder")
    set_default(false)
    set_showmenu(true)
    set_description("Use default Jetson encoder from WebRTC")
option_end()

option("enable_encode_perf_stats")
    set_default(false)
    set_showmenu(true)
    set_description("Enable encode performance statistics")
option_end()

option("build_examples")
    set_default(true)
    set_showmenu(true)
    set_description("Build Qt demo target p2p_qt")
option_end()

local function is_aarch64_arch()
    return is_arch("aarch64", "arm64")
end

local function resolve_asio_include()
    local configured_path = get_config("asio_path")
    if configured_path and #configured_path > 0 then
        if os.isfile(path.join(configured_path, "asio.hpp")) then
            return configured_path
        end
        if os.isfile(path.join(configured_path, "asio", "asio.hpp")) then
            return path.join(configured_path, "asio")
        end
        assert(false, string.format("invalid --asio_path: %s", configured_path))
    end

    local env_path = os.getenv("ASIO_PATH")
    if env_path and #env_path > 0 then
        if os.isfile(path.join(env_path, "asio.hpp")) then
            return env_path
        end
        if os.isfile(path.join(env_path, "asio", "asio.hpp")) then
            return path.join(env_path, "asio")
        end
    end

    local candidates = {
        "/usr/include",
        "/usr/local/include",
        "/usr/include/asio",
        "/usr/local/include/asio"
    }
    for _, candidate in ipairs(candidates) do
        if os.isfile(path.join(candidate, "asio.hpp")) then
            return candidate
        end
        if os.isfile(path.join(candidate, "asio", "asio.hpp")) then
            return path.join(candidate, "asio")
        end
    end
    return nil
end

local function require_asio_include()
    local include_dir = resolve_asio_include()
    if not include_dir then
        assert(false, "Standalone Asio not found. Install libasio-dev or pass --asio_path=/path/to/asio/include")
    end
    return include_dir
end

local function webrtc_root_dir()
    if on_windows() then
        return path.join("third_party", "webrtc", "webrtc-win")
    end

    if on_linux() then
        if is_aarch64_arch() then
            if get_config("use_default_jetson_encoder") then
                return path.join("third_party", "webrtc", "webrtc-jetson-default")
            end
            return path.join("third_party", "webrtc", "webrtc-jetson")
        end
        return path.join("third_party", "webrtc", "webrtc-linux")
    end

    assert(false, "unsupported platform for WebRTC")
end

local function add_webrtc_config()
    local root_dir = webrtc_root_dir()
    add_includedirs(
        path.join(root_dir, "include"),
        path.join(root_dir, "include", "third_party", "abseil-cpp"),
        path.join(root_dir, "include", "third_party", "jsoncpp", "source", "include"),
        path.join(root_dir, "include", "third_party", "jsoncpp", "generated"),
        path.join(root_dir, "include", "third_party", "libyuv", "include"),
        path.join(root_dir, "include", "third_party", "boringssl", "src", "include")
    )

    local lib_dir = path.join(root_dir, "lib")
    add_linkdirs(lib_dir)

    local debug_lib_exists = false
    if on_windows() then
        debug_lib_exists = os.isfile(path.join(lib_dir, "webrtcd.lib"))
    else
        debug_lib_exists = os.isfile(path.join(lib_dir, "libwebrtcd.a"))
    end

    if is_mode("debug") and debug_lib_exists and not (is_aarch64_arch() and get_config("use_default_jetson_encoder")) then
        add_links("webrtcd")
    else
        add_links("webrtc")
    end
end

local function add_simple_web_config()
    add_defines("USE_STANDALONE_ASIO")
    add_includedirs(
        "third_party/Simple-Web-Server",
        "third_party/Simple-WebSocket-Server",
        require_asio_include()
    )

    if on_windows() then
        add_syslinks("ws2_32", "wsock32")
    else
        add_syslinks("pthread")
    end
end

local function add_vtslog_config()
    add_includedirs("third_party/vtslog/include")

    if on_windows() then
        local cfg = is_mode("debug") and "Debug" or "Release"
        add_linkdirs(path.join("third_party", "vtslog", "lib", "windows", cfg))
        add_links("vtslog")
    elseif on_linux() then
        local arch_dir = is_aarch64_arch() and "aarch64" or "x64"
        add_linkdirs(path.join("third_party", "vtslog", "lib", arch_dir))
        add_links("vtslog", "minizip")
    else
        assert(false, "unsupported platform for vtslog")
    end
end

local function add_json_config()
    add_includedirs("third_party/json/include")
end

local function add_openssl_config()
    if on_windows() then
        local openssl_dir = path.join("third_party", "Openssl-Win64")
        add_includedirs(path.join(openssl_dir, "include"))
        add_linkdirs(path.join(openssl_dir, "lib"))
        add_links("libcrypto", "libssl")
    else
        add_syslinks("ssl", "crypto")
    end
end

local function add_nvcodec_config()
    local sdk_root = path.join("third_party", "Video_Codec_SDK_11.0.10")
    add_includedirs(
        path.join(sdk_root, "Interface"),
        path.join(sdk_root, "Samples", "NvCodec"),
        path.join(sdk_root, "Samples", "Utils")
    )

    if on_windows() then
        add_linkdirs(path.join(sdk_root, "Lib", "x64"))
        add_links("nvcuvid", "nvencodeapi")
    elseif on_linux() then
        local arch_dir = is_aarch64_arch() and "aarch64" or "x86_64"
        add_linkdirs(path.join(sdk_root, "Lib", "linux", "stubs", arch_dir))
        add_links("nvcuvid", "nvidia-encode")
    else
        assert(false, "unsupported platform for NvCodec")
    end
end

local function add_cuda_driver_config()
    local cuda_include_candidates = {
        "/usr/local/cuda/include",
        "/usr/include"
    }
    for _, include_dir in ipairs(cuda_include_candidates) do
        if os.isfile(path.join(include_dir, "cuda.h")) then
            add_includedirs(include_dir)
            break
        end
    end

    if on_windows() then
        add_links("nvcuda")
    else
        add_links("cuda")
    end
end

local vtsrtc_target = is_aarch64_arch() and "vtsrtc_aarch64" or "vtsrtc"

target(vtsrtc_target)
    set_kind("shared")
    if is_aarch64_arch() then
        set_basename("vtsrtc.aarch64")
    else
        set_basename("vtsrtc")
    end

    add_includedirs("vtsrtc/src", {public = true})
    add_files(
        "vtsrtc/src/log/log_manager.cpp",
        "vtsrtc/src/log/log_webrtc_hook.cpp",
        "vtsrtc/src/rtc_device_manager.cpp",
        "vtsrtc/src/rtc_connection.cpp",
        "vtsrtc/src/rtc_connection_manager.cpp",
        "vtsrtc/src/rtc.cpp",
        "vtsrtc/src/c_rtc.cpp",
        "vtsrtc/src/video/encode/rtc_encoder_factory.cpp",
        "vtsrtc/src/statistics/rtc_statistics.cpp",
        "vtsrtc/src/packet/pack.cpp",
        "vtsrtc/src/packet/packetizer.cpp",
        "vtsrtc/src/packet/depacketizer.cpp"
    )

    if get_config("use_default_jetson_encoder") then
        add_defines("USE_DEFAULT_JETSON_ENCODER")
    end

    if on_windows() then
        add_defines("WEBRTC_WIN", "NOMINMAX", "WIN32_LEAN_AND_MEAN", "RTC_DLL_EXPORTS")
        add_syslinks("winmm", "Secur32", "Msdmo", "Dmoguids", "wmcodecdspuuid", "Strmiids")
    elseif on_linux() then
        add_defines("WEBRTC_LINUX", "WEBRTC_POSIX")
    end

    add_simple_web_config()
    add_json_config()
    add_vtslog_config()
    add_webrtc_config()

    if is_aarch64_arch() then
        add_files(
            "vtsrtc/src/video/encode/nvidia-jetson/jetsonh264_encoder_impl.cpp",
            "vtsrtc/src/video/encode/nvidia-jetson/jetson_encoder.cpp",
            "vtsrtc/src/video/encode/nvidia-jetson/nvmpi_enc.cpp",
            "/usr/src/jetson_multimedia_api/samples/common/classes/NvVideoEncoder.cpp",
            "/usr/src/jetson_multimedia_api/samples/common/classes/NvV4l2Element.cpp",
            "/usr/src/jetson_multimedia_api/samples/common/classes/NvV4l2ElementPlane.cpp",
            "/usr/src/jetson_multimedia_api/samples/common/classes/NvElementProfiler.cpp",
            "/usr/src/jetson_multimedia_api/samples/common/classes/NvBuffer.cpp",
            "/usr/src/jetson_multimedia_api/samples/common/classes/NvElement.cpp",
            "/usr/src/jetson_multimedia_api/samples/common/classes/NvLogging.cpp"
        )
        add_includedirs("/usr/src/jetson_multimedia_api/include")

        if get_config("enable_encode_perf_stats") then
            add_defines("ENABLE_ENCODE_PERF_STATS=1")
        else
            add_defines("ENABLE_ENCODE_PERF_STATS=0")
        end

        add_syslinks("v4l2", "nvbufsurface", "nvbufsurftransform", "X11")
        add_linkdirs("/usr/lib/aarch64-linux-gnu/tegra")
    else
        add_files(
            "vtsrtc/src/video/decode/rtc_decoder_factory.cpp",
            "vtsrtc/src/video/encode/nvidia/nvh264_encoder_impl.cpp",
            "vtsrtc/src/video/decode/nvidia/nvh264_decoder_impl.cpp",
            "third_party/Video_Codec_SDK_11.0.10/Samples/NvCodec/NvEncoder/NvEncoder.cpp",
            "third_party/Video_Codec_SDK_11.0.10/Samples/NvCodec/NvEncoder/NvEncoderCuda.cpp",
            "third_party/Video_Codec_SDK_11.0.10/Samples/NvCodec/NvDecoder/NvDecoder.cpp"
        )

        add_cuda_driver_config()
        add_nvcodec_config()
    end

    add_installfiles("vtsrtc/src/c_rtc.h", {prefixdir = "vtsrtc/include"})
    add_installfiles("test_data/rtc.cfg", {prefixdir = "vtsrtc"})

target("signaling-server")
    set_kind("binary")
    set_languages("cxx17")

    add_files(
        "signaling-server/src/log_manager.cpp",
        "signaling-server/src/http_controller.cpp",
        "signaling-server/src/ws_controller.cpp",
        "signaling-server/src/main.cpp"
    )

    add_includedirs("signaling-server/src")
    add_simple_web_config()
    add_json_config()
    add_vtslog_config()
    add_openssl_config()

if get_config("build_examples") then
    target("p2p_qt")
        set_kind("binary")
        add_rules("qt.widgetapp")
        add_deps(vtsrtc_target)

        add_files(
            "vtsrtc/examples/rtc_audiorender.cpp",
            "vtsrtc/examples/rtc_videorender.cpp",
            "vtsrtc/examples/rtc_widget.cpp",
            "vtsrtc/examples/main.cpp",
            "vtsrtc/examples/rtc_audiorender.h",
            "vtsrtc/examples/rtc_videorender.h",
            "vtsrtc/examples/rtc_widget.h"
        )

        add_includedirs("vtsrtc/examples", "vtsrtc/src")
        add_vtslog_config()
        if on_linux() then
            add_syslinks("pthread")
        end
end
