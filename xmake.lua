set_project("rtc-solutions")
set_version("0.5.0")

add_rules("mode.debug", "mode.release")
set_languages("cxx14")
add_requires("imgui v1.92.6", {configs = {opengl3 = true, sdl3 = true}})
add_requires("libsdl3 3.4.2")
add_requires("asio 1.32.0")

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

option("use_libvtslog")
    set_default(false)
    set_showmenu(true)
    set_description("Use external libvtslog for vtsrtc logging")
option_end()

option("build_examples")
    set_default(true)
    set_showmenu(true)
    set_description("Build ImGui demo target p2p_imgui")
option_end()

local function is_aarch64_arch()
    return is_arch("aarch64", "arm64")
end

local reported_failures = {}
local function fail(format, ...)
    local message = string.format(format, ...)
    if not reported_failures[message] then
        print("error: " .. message)
        reported_failures[message] = true
    end
    return nil
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

    return fail("unsupported platform for WebRTC")
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
    add_defines("USE_STANDALONE_ASIO", "ASIO_STANDALONE")
    add_includedirs(
        "third_party/Simple-Web-Server",
        "third_party/Simple-WebSocket-Server"
    )
    add_packages("asio")

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
        return fail("unsupported platform for vtslog")
    end
end

local function add_optional_vtslog_config()
    if get_config("use_libvtslog") then
        add_vtslog_config()
    end
end

local function copy_optional_vtslog_runtime(batchcmds, target)
    if not get_config("use_libvtslog") or not on_linux() then
        return
    end
    local arch_dir = is_aarch64_arch() and "aarch64" or "x64"
    local vtslog_dir = path.join(os.projectdir(), "third_party", "vtslog", "lib", arch_dir)
    batchcmds:cp(path.join(vtslog_dir, "libvtslog.so"), target:targetdir())
    batchcmds:cp(path.join(vtslog_dir, "libminizip.so"), target:targetdir())
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
        return fail("unsupported platform for NvCodec")
    end
end

local function add_jetson_h264_encoder_config()
    if not os.isdir("/usr/src/jetson_multimedia_api/include") then
        return fail("jetson multimedia api headers not found at /usr/src/jetson_multimedia_api/include")
    end

    add_includedirs("/usr/src/jetson_multimedia_api/include")
    add_linkdirs("/usr/lib/aarch64-linux-gnu/tegra")
    add_syslinks("v4l2", "nvbufsurface", "nvbufsurftransform", "X11")
    add_files(
        "vtsrtc/src/video/encode/nvidia-jetson/jetsonh264_encoder_impl.cpp",
        "vtsrtc/src/video/encode/nvidia-jetson/jetson_encoder.cpp",
        "vtsrtc/src/video/encode/nvidia-jetson/nvmpi_enc.cpp",
        "vtsrtc/src/video/encode/nvidia-jetson/NvVideoEncoder.cpp",
        "/usr/src/jetson_multimedia_api/samples/common/classes/NvV4l2Element.cpp",
        "/usr/src/jetson_multimedia_api/samples/common/classes/NvV4l2ElementPlane.cpp",
        "/usr/src/jetson_multimedia_api/samples/common/classes/NvElementProfiler.cpp",
        "/usr/src/jetson_multimedia_api/samples/common/classes/NvBuffer.cpp",
        "/usr/src/jetson_multimedia_api/samples/common/classes/NvElement.cpp",
        "/usr/src/jetson_multimedia_api/samples/common/classes/NvLogging.cpp"
    )

    return true
end

local function add_rasp_h264_encoder_config()
    if not os.isfile("/usr/include/gstreamer-1.0/gst/gst.h") then
        return false
    end

    add_includedirs(
        "/usr/include/gstreamer-1.0",
        "/usr/include/orc-0.4",
        "/usr/include/glib-2.0",
        "/usr/lib/aarch64-linux-gnu/glib-2.0/include"
    )
    add_links("gstapp-1.0", "gstvideo-1.0", "gstbase-1.0", "gstreamer-1.0", "gobject-2.0", "glib-2.0")
    add_files(
        "vtsrtc/src/video/encode/rasp/rasp_h264_encoder_impl.cpp"
    )
    add_defines("VTSRTC_HAS_GSTREAMER")
    return true
end

local function add_cuda_driver_config()
    local cuda_include_candidates = {}
    local cuda_path = os.getenv("CUDA_PATH")
    local cuda_home = os.getenv("CUDA_HOME")

    if on_windows() then
        if cuda_path then
            table.insert(cuda_include_candidates, path.join(cuda_path, "include"))
        end
        if cuda_home then
            table.insert(cuda_include_candidates, path.join(cuda_home, "include"))
        end
        for _, include_dir in ipairs(os.dirs("C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v*/include")) do
            table.insert(cuda_include_candidates, include_dir)
        end
    else
        if cuda_path then
            table.insert(cuda_include_candidates, path.join(cuda_path, "include"))
        end
        if cuda_home then
            table.insert(cuda_include_candidates, path.join(cuda_home, "include"))
        end
        table.insert(cuda_include_candidates, "/usr/local/cuda/include")
        table.insert(cuda_include_candidates, "/usr/include")
    end

    local found_cuda_include = nil
    for _, include_dir in ipairs(cuda_include_candidates) do
        if os.isfile(path.join(include_dir, "cuda.h")) then
            found_cuda_include = include_dir
            break
        end
    end

    if not found_cuda_include then
        return false
    end

    add_includedirs(found_cuda_include)

    if on_windows() then
        local cuda_lib_candidates = {}
        if cuda_path then
            table.insert(cuda_lib_candidates, path.join(cuda_path, "lib", "x64"))
        end
        if cuda_home then
            table.insert(cuda_lib_candidates, path.join(cuda_home, "lib", "x64"))
        end
        for _, lib_dir in ipairs(os.dirs("C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v*/lib/x64")) do
            table.insert(cuda_lib_candidates, lib_dir)
        end

        local found_cuda_libdir = nil
        for _, lib_dir in ipairs(cuda_lib_candidates) do
            if os.isfile(path.join(lib_dir, "cuda.lib")) then
                found_cuda_libdir = lib_dir
                break
            end
        end

        if not found_cuda_libdir then
            return false
        end

        add_linkdirs(found_cuda_libdir)
        add_links("cuda")
    else
        add_links("cuda")
    end

    add_defines("VTSRTC_HAS_CUDA_DRIVER=1")
    return true
end

local function add_cuda_runtime_config()
    local cuda_include_candidates = {}
    local cuda_lib_candidates = {}
    local cuda_path = os.getenv("CUDA_PATH")
    local cuda_home = os.getenv("CUDA_HOME")

    if cuda_path then
        table.insert(cuda_include_candidates, path.join(cuda_path, "include"))
        if on_windows() then
            table.insert(cuda_lib_candidates, path.join(cuda_path, "lib", "x64"))
        else
            table.insert(cuda_lib_candidates, path.join(cuda_path, "lib64"))
        end
    end
    if cuda_home then
        table.insert(cuda_include_candidates, path.join(cuda_home, "include"))
        if on_windows() then
            table.insert(cuda_lib_candidates, path.join(cuda_home, "lib", "x64"))
        else
            table.insert(cuda_lib_candidates, path.join(cuda_home, "lib64"))
        end
    end

    if on_windows() then
        for _, include_dir in ipairs(os.dirs("C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v*/include")) do
            table.insert(cuda_include_candidates, include_dir)
        end
        for _, lib_dir in ipairs(os.dirs("C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v*/lib/x64")) do
            table.insert(cuda_lib_candidates, lib_dir)
        end
    else
        table.insert(cuda_include_candidates, "/usr/local/cuda/include")
        table.insert(cuda_lib_candidates, "/usr/local/cuda/lib64")
    end

    local found_cuda_include = nil
    for _, include_dir in ipairs(cuda_include_candidates) do
        if os.isfile(path.join(include_dir, "cuda_runtime.h")) then
            found_cuda_include = include_dir
            break
        end
    end
    if not found_cuda_include then
        return false
    end

    local found_cuda_libdir = nil
    for _, lib_dir in ipairs(cuda_lib_candidates) do
        if on_windows() then
            if os.isfile(path.join(lib_dir, "cudart.lib")) then
                found_cuda_libdir = lib_dir
                break
            end
        else
            if os.isfile(path.join(lib_dir, "libcudart.so")) then
                found_cuda_libdir = lib_dir
                break
            end
        end
    end
    if not found_cuda_libdir then
        return false
    end

    add_includedirs(found_cuda_include)
    add_linkdirs(found_cuda_libdir)
    add_links("cudart")
    if on_linux() then
        add_rpathdirs(found_cuda_libdir)
    end
    return true
end

local function add_linux_runtime_rpath()
    if on_linux() then
        local arch_dir = is_aarch64_arch() and "aarch64" or "x64"
        add_ldflags("-Wl,--disable-new-dtags", {force = true})
        add_rpathdirs("$ORIGIN")
        add_rpathdirs(path.join(os.projectdir(), "third_party", "vtslog", "lib", arch_dir))
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

    if get_config("enable_encode_perf_stats") then
        add_defines("ENABLE_ENCODE_PERF_STATS=1")
    else
        add_defines("ENABLE_ENCODE_PERF_STATS=0")
    end

    if get_config("use_libvtslog") then
        add_defines("VTSRTC_USE_LIBVTSLOG=1")
    else
        add_defines("VTSRTC_USE_LIBVTSLOG=0")
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
        add_syslinks("winmm", "Secur32", "Msdmo", "Dmoguids", "wmcodecdspuuid", "Strmiids", "Advapi32")
    elseif on_linux() then
        add_defines("WEBRTC_LINUX", "WEBRTC_POSIX")
    end

    add_simple_web_config()
    add_json_config()
    add_optional_vtslog_config()
    add_webrtc_config()
    add_linux_runtime_rpath()

    if is_aarch64_arch() then
        if not get_config("use_default_jetson_encoder") then
            add_jetson_h264_encoder_config()
            add_rasp_h264_encoder_config()
        end
    else
        add_files(
            "vtsrtc/src/video/decode/rtc_decoder_factory.cpp"
        )

        if add_cuda_driver_config() then
            add_files(
                "vtsrtc/src/video/encode/nvidia/nvh264_encoder_impl.cpp",
                "vtsrtc/src/video/decode/nvidia/nvh264_decoder_impl.cpp",
                "third_party/Video_Codec_SDK_11.0.10/Samples/NvCodec/NvEncoder/NvEncoder.cpp",
                "third_party/Video_Codec_SDK_11.0.10/Samples/NvCodec/NvEncoder/NvEncoderCuda.cpp",
                "third_party/Video_Codec_SDK_11.0.10/Samples/NvCodec/NvDecoder/NvDecoder.cpp"
            )
            add_nvcodec_config()
        else
            add_defines("VTSRTC_HAS_CUDA_DRIVER=0")
            print("warning: CUDA toolkit not found, NVENC/NVDEC sources are disabled for this build")
        end
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
    add_linux_runtime_rpath()

    after_buildcmd(function(target, batchcmds)
        if on_linux() then
            local arch_dir = is_aarch64_arch() and "aarch64" or "x64"
            local vtslog_dir = path.join(os.projectdir(), "third_party", "vtslog", "lib", arch_dir)
            batchcmds:cp(path.join(vtslog_dir, "libvtslog.so"), target:targetdir())
            batchcmds:cp(path.join(vtslog_dir, "libminizip.so"), target:targetdir())
            batchcmds:cp(path.join(os.projectdir(), "test_data", "web"), path.join(target:targetdir(), "web"))
            batchcmds:cp(path.join(os.projectdir(), "test_data", "signaling-server.cfg"), target:targetdir())
        end
    end)

if get_config("build_examples") then
    target("p2p_imgui")
        set_kind("binary")
        add_deps(vtsrtc_target)
        add_packages("imgui")

        add_files(
            "vtsrtc/examples_imgui/main.cpp"
        )

        add_includedirs("vtsrtc/src")
        add_linux_runtime_rpath()
        if on_windows() then
            add_syslinks("opengl32")
        elseif on_linux() then
            add_syslinks("pthread", "GL", "dl", "asound")
        end

        after_buildcmd(function(target, batchcmds)
            if on_linux() then
                copy_optional_vtslog_runtime(batchcmds, target)
                batchcmds:cp(path.join(os.projectdir(), "test_data", "rtc.cfg"), target:targetdir())
                batchcmds:cp(path.join(os.projectdir(), "test_data", "8k16bit.pcm"), target:targetdir())
                batchcmds:cp(path.join(os.projectdir(), "test_data", "zjlabs.yuv"), target:targetdir())
                batchcmds:cp(path.join(os.projectdir(), "test_data", "messagefile.txt"), target:targetdir())
            end
        end)

    if on_linux() then
        target("rtc_camera_headless")
            set_kind("binary")
            add_deps(vtsrtc_target)

            add_files(
                "vtsrtc/examples_imgui/rtc_camera_headless.cpp",
                "vtsrtc/examples_imgui/rtc_camera_common.cpp",
                "vtsrtc/examples_imgui/uyvy_v4l2_camera.cpp",
                "vtsrtc/examples_imgui/rtc_headless_session.cpp",
                "vtsrtc/examples_imgui/uyvy_to_i420_cuda.cu"
            )

            add_includedirs("vtsrtc/src")
            add_linux_runtime_rpath()
            add_syslinks("pthread")
            add_cuflags("--std=c++14")

            if not add_cuda_runtime_config() then
                raise("CUDA runtime not found for rtc_camera_headless")
            end

            after_buildcmd(function(target, batchcmds)
                copy_optional_vtslog_runtime(batchcmds, target)
                batchcmds:cp(path.join(os.projectdir(), "test_data", "rtc.cfg"), target:targetdir())
            end)

        target("rtc_receiver_headless")
            set_kind("binary")
            add_deps(vtsrtc_target)

            add_files(
                "vtsrtc/examples_imgui/rtc_receiver_headless.cpp",
                "vtsrtc/examples_imgui/rtc_camera_common.cpp",
                "vtsrtc/examples_imgui/rtc_headless_session.cpp"
            )

            add_includedirs("vtsrtc/src")
            add_linux_runtime_rpath()
            add_syslinks("pthread")

            after_buildcmd(function(target, batchcmds)
                copy_optional_vtslog_runtime(batchcmds, target)
                batchcmds:cp(path.join(os.projectdir(), "test_data", "rtc.cfg"), target:targetdir())
            end)
    end
end

if on_linux() then
    target("v4l2_camera_test")
        set_kind("binary")
        add_files("vtsrtc/examples/v4l2_camera_test.cpp")
        add_packages("libsdl3")
        add_syslinks("v4l2", "pthread", "dl")
end
