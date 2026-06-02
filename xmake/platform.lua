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
