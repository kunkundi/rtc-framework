local reported_failures = {}

function vtsrtc_on_windows()
    return is_plat("windows") or is_host("windows")
end

function vtsrtc_on_linux()
    return is_plat("linux") or is_host("linux")
end

function vtsrtc_configure_targetdir()
    if vtsrtc_on_windows() then
        set_targetdir("$(builddir)/runtime/$(mode)")
    else
        set_targetdir("$(builddir)/runtime")
    end
end

function vtsrtc_fail(format, ...)
    local message = string.format(format, ...)
    if not reported_failures[message] then
        print("error: " .. message)
        reported_failures[message] = true
    end
    return nil
end

function vtsrtc_get_bool_config(name, default)
    local value = get_config(name)
    if value == nil then
        return default
    end
    return value
end

function vtsrtc_is_aarch64_arch()
    local arch = get_config("arch") or os.arch()
    return arch == "aarch64" or arch == "arm64"
end

function vtsrtc_target_name()
    return vtsrtc_is_aarch64_arch() and "vtsrtc_aarch64" or "vtsrtc"
end

function vtsrtc_path(...)
    return path.join(os.projectdir(), ...)
end

function vtsrtc_add_linux_runtime_rpath()
    if vtsrtc_on_linux() then
        add_ldflags("-Wl,--disable-new-dtags", {force = true})
        add_rpathdirs("$ORIGIN")
    end
end

function vtsrtc_add_linux_symbol_visibility(version_script)
    if not vtsrtc_on_linux() then
        return
    end

    add_cxflags("-fvisibility=hidden", "-fvisibility-inlines-hidden", {force = true})
    add_shflags(
        "-Wl,--exclude-libs,ALL",
        "-Wl,--version-script=" .. path.absolute(version_script),
        {force = true}
    )
end

-- Returns expected vtsrtc shared library path under lib/.
function vtsrtc_prebuilt_lib_path()
    local lib_name = vtsrtc_target_name()
    local lib_dir = path.join(os.projectdir(), "lib")
    if is_plat("windows") or is_host("windows") then
        return path.join(lib_dir, lib_name .. ".dll")
    else
        return path.join(lib_dir, "lib" .. lib_name .. ".so")
    end
end

-- Returns true when a pre-built vtsrtc shared library exists in lib/.
function vtsrtc_has_prebuilt_lib()
    return os.isfile(vtsrtc_prebuilt_lib_path())
end

-- Client targets call this instead of add_deps(vtsrtc_target_name()).
-- Uses pre-built lib from lib/ when available; otherwise compiles vtsrtc from source.
function vtsrtc_add_client_dependency()
    local prebuilt = vtsrtc_prebuilt_lib_path()
    if os.isfile(prebuilt) then
        add_linkdirs(path.join(os.projectdir(), "lib"))
        add_links(vtsrtc_target_name())
        add_includedirs(path.join(os.projectdir(), "vtsrtc", "src"))
        after_buildcmd(function(target, batchcmds)
            batchcmds:cp(prebuilt, target:targetdir())
        end)
    else
        add_deps(vtsrtc_target_name())
    end
end
