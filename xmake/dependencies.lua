add_requires("imgui v1.92.6", {configs = {opengl3 = true, sdl3 = true}})
add_requires("asio 1.32.0")
add_requires("spdlog 1.14.1")
add_requires("nanopb 0.4.9", {configs = {generator = true}})

if get_config("enable_yolo") then
    add_requires("onnxruntime 1.22.0", {configs = {shared = true}})
end

function vtsrtc_webrtc_root_dir()
    if vtsrtc_on_windows() then
        return vtsrtc_path("third_party", "webrtc", "webrtc-win")
    end

    if vtsrtc_on_linux() then
        if vtsrtc_is_aarch64_arch() then
            if get_config("use_default_jetson_encoder") then
                return vtsrtc_path("third_party", "webrtc", "webrtc-jetson-default")
            end
            return vtsrtc_path("third_party", "webrtc", "webrtc-jetson")
        end
        return vtsrtc_path("third_party", "webrtc", "webrtc-linux")
    end

    return vtsrtc_fail("unsupported platform for WebRTC")
end

function vtsrtc_add_webrtc_config()
    local root_dir = vtsrtc_webrtc_root_dir()
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
    if vtsrtc_on_windows() then
        debug_lib_exists = os.isfile(path.join(lib_dir, "webrtcd.lib"))
    else
        debug_lib_exists = os.isfile(path.join(lib_dir, "libwebrtcd.a"))
    end

    if is_mode("debug") and debug_lib_exists and not (vtsrtc_is_aarch64_arch() and get_config("use_default_jetson_encoder")) then
        add_links("webrtcd")
    else
        add_links("webrtc")
    end
end

function vtsrtc_add_simple_web_config()
    add_defines("USE_STANDALONE_ASIO", "ASIO_STANDALONE")
    add_includedirs(
        vtsrtc_path("third_party", "Simple-Web-Server"),
        vtsrtc_path("third_party", "Simple-WebSocket-Server")
    )
    add_packages("asio")

    if vtsrtc_on_windows() then
        add_syslinks("ws2_32", "wsock32")
    else
        add_syslinks("pthread")
    end
end

function vtsrtc_add_spdlog_config()
    add_packages("spdlog")
end

function vtsrtc_add_json_config()
    add_includedirs(vtsrtc_path("third_party", "json", "include"))
end

function vtsrtc_add_openssl_config()
    if vtsrtc_on_windows() then
        local openssl_dir = vtsrtc_path("third_party", "Openssl-Win64")
        add_includedirs(path.join(openssl_dir, "include"))
        add_linkdirs(path.join(openssl_dir, "lib"))
        add_links("libcrypto", "libssl")
    else
        add_syslinks("ssl", "crypto")
    end
end
