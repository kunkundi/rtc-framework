function vtsrtc_add_nvcodec_config()
    local sdk_root = vtsrtc_path("third_party", "Video_Codec_SDK_11.0.10")
    add_includedirs(
        path.join(sdk_root, "Interface"),
        path.join(sdk_root, "Samples", "NvCodec"),
        path.join(sdk_root, "Samples", "Utils")
    )

    if vtsrtc_on_windows() then
        add_linkdirs(path.join(sdk_root, "Lib", "x64"))
        add_links("nvcuvid", "nvencodeapi")
        add_syslinks("delayimp")
        add_ldflags(
            "/DELAYLOAD:nvcuda.dll",
            "/DELAYLOAD:nvcuvid.dll",
            "/DELAYLOAD:nvEncodeAPI64.dll",
            {force = true}
        )
    elseif vtsrtc_on_linux() then
        local arch_dir = vtsrtc_is_aarch64_arch() and "aarch64" or "x86_64"
        add_linkdirs(path.join(sdk_root, "Lib", "linux", "stubs", arch_dir))
        add_links("nvcuvid", "nvidia-encode")
    else
        return vtsrtc_fail("unsupported platform for NvCodec")
    end
end

function vtsrtc_add_jetson_multimedia_api_compat_defines()
    local include_dir = "/usr/src/jetson_multimedia_api/include"
    local include_flag = "-I" .. include_dir

    check_cxxsnippets(
        "VTSRTC_JETSON_NVVIDEOENCODER_HAS_DEVNODE_CTOR",
        [[
            NvVideoEncoder::NvVideoEncoder(const char *name, const char *dev_node, int flags)
                : NvV4l2Element(name, dev_node, flags, valid_fields) {}
        ]],
        {
            name = "jetson_nvvideoencoder_has_devnode_ctor",
            includes = "NvVideoEncoder.h",
            languages = "cxx14",
            cxflags = include_flag
        }
    )

    check_cxxsnippets(
        "VTSRTC_JETSON_NVVIDEOENCODER_HAS_PLAIN_CTOR",
        [[
            NvVideoEncoder::NvVideoEncoder(const char *name, int flags)
                : NvV4l2Element(name, "/dev/null", flags, valid_fields) {}
        ]],
        {
            name = "jetson_nvvideoencoder_has_plain_ctor",
            includes = "NvVideoEncoder.h",
            languages = "cxx14",
            cxflags = include_flag
        }
    )

    check_cxxsnippets(
        "VTSRTC_JETSON_NVVIDEOENCODER_HAS_COLORSPACE_OUTPUT_FORMAT",
        [[
            int NvVideoEncoder::setOutputPlaneFormat(uint32_t pixfmt, uint32_t width,
                    uint32_t height, enum v4l2_colorspace cs)
            {
                (void)pixfmt;
                (void)width;
                (void)height;
                (void)cs;
                return 0;
            }
        ]],
        {
            name = "jetson_nvvideoencoder_has_colorspace_output_format",
            includes = "NvVideoEncoder.h",
            languages = "cxx14",
            cxflags = include_flag
        }
    )

    check_cxxsnippets(
        "VTSRTC_JETSON_NVVIDEOENCODER_HAS_PLAIN_OUTPUT_FORMAT",
        [[
            int NvVideoEncoder::setOutputPlaneFormat(uint32_t pixfmt, uint32_t width,
                    uint32_t height)
            {
                (void)pixfmt;
                (void)width;
                (void)height;
                return 0;
            }
        ]],
        {
            name = "jetson_nvvideoencoder_has_plain_output_format",
            includes = "NvVideoEncoder.h",
            languages = "cxx14",
            cxflags = include_flag
        }
    )

    return true
end

function vtsrtc_add_jetson_h264_encoder_config()
    if not os.isdir("/usr/src/jetson_multimedia_api/include") then
        return vtsrtc_fail("jetson multimedia api headers not found at /usr/src/jetson_multimedia_api/include")
    end

    if not vtsrtc_add_jetson_multimedia_api_compat_defines() then
        return nil
    end

    add_includedirs("/usr/src/jetson_multimedia_api/include")
    add_linkdirs("/usr/lib/aarch64-linux-gnu/tegra")
    add_syslinks("v4l2", "nvbufsurface", "nvbufsurftransform", "X11")
    add_files(
        vtsrtc_path("vtsrtc", "src", "video", "encode", "nvidia-jetson", "jetsonh264_encoder_impl.cpp"),
        vtsrtc_path("vtsrtc", "src", "video", "encode", "nvidia-jetson", "jetson_encoder.cpp"),
        vtsrtc_path("vtsrtc", "src", "video", "encode", "nvidia-jetson", "NvVideoEncoder.cpp"),
        "/usr/src/jetson_multimedia_api/samples/common/classes/NvV4l2Element.cpp",
        "/usr/src/jetson_multimedia_api/samples/common/classes/NvV4l2ElementPlane.cpp",
        "/usr/src/jetson_multimedia_api/samples/common/classes/NvElementProfiler.cpp",
        "/usr/src/jetson_multimedia_api/samples/common/classes/NvBuffer.cpp",
        "/usr/src/jetson_multimedia_api/samples/common/classes/NvElement.cpp",
        "/usr/src/jetson_multimedia_api/samples/common/classes/NvLogging.cpp"
    )

    return true
end

function vtsrtc_add_rasp_h264_encoder_config()
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
        vtsrtc_path("vtsrtc", "src", "video", "encode", "rasp", "rasp_h264_encoder_impl.cpp")
    )
    add_defines("VTSRTC_HAS_GSTREAMER")
    return true
end

function vtsrtc_add_cuda_driver_config()
    if not vtsrtc_get_bool_config("enable_cuda", true) then
        return false
    end

    local cuda_include_candidates = {}
    local cuda_path = os.getenv("CUDA_PATH")
    local cuda_home = os.getenv("CUDA_HOME")

    if vtsrtc_on_windows() then
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

    if vtsrtc_on_windows() then
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

function vtsrtc_add_cuda_runtime_config()
    if not vtsrtc_get_bool_config("enable_cuda", true) then
        return false
    end

    local cuda_include_candidates = {}
    local cuda_lib_candidates = {}
    local cuda_path = os.getenv("CUDA_PATH")
    local cuda_home = os.getenv("CUDA_HOME")

    if cuda_path then
        table.insert(cuda_include_candidates, path.join(cuda_path, "include"))
        if vtsrtc_on_windows() then
            table.insert(cuda_lib_candidates, path.join(cuda_path, "lib", "x64"))
        else
            table.insert(cuda_lib_candidates, path.join(cuda_path, "lib64"))
        end
    end
    if cuda_home then
        table.insert(cuda_include_candidates, path.join(cuda_home, "include"))
        if vtsrtc_on_windows() then
            table.insert(cuda_lib_candidates, path.join(cuda_home, "lib", "x64"))
        else
            table.insert(cuda_lib_candidates, path.join(cuda_home, "lib64"))
        end
    end

    if vtsrtc_on_windows() then
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
        if vtsrtc_on_windows() then
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
    if vtsrtc_on_linux() then
        add_rpathdirs(found_cuda_libdir)
    end
    return true
end
