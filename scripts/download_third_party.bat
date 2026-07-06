@echo off
chcp 65001 >nul
rem =============================================================================
rem  下载 third_party 大文件（Windows 版）
rem
rem  使用方法：
rem    1. 先在 GitHub Releases 创建 Release 并上传 packages/ 下的 tar.gz 文件
rem    2. 修改下方 RELEASE_URL 为实际下载地址
rem    3. 运行: scripts\download_third_party.bat
rem
rem  Release 中应有的文件:
rem    webrtc-linux.tar.gz
rem    webrtc-jetson.tar.gz
rem    webrtc-jetson-default.tar.gz
rem    webrtc-win.tar.gz
rem    Video_Codec_SDK_11.0.10.tar.gz
rem    zjlabs.yuv.tar.gz
rem =============================================================================

setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "PROJECT_ROOT=%SCRIPT_DIR%.."

rem ==============================
rem 配置：固定 Release 下载地址，更新依赖时改版本号即可
rem ==============================
set "RELEASE_URL=https://github.com/kunkundi/rtc-framework/releases/download/v1.0.0"

rem 临时目录
set "TEMP_DIR=%TEMP%\rtc_thirdparty"
if not exist "%TEMP_DIR%" mkdir "%TEMP_DIR%"

echo ==========================================
echo   下载 third_party 依赖文件 (Windows^)
echo ==========================================
echo.
echo Release URL: %RELEASE_URL%
echo.

rem ==============================
rem 检查 tar 是否可用（Win10 1803+ 自带）
rem ==============================
where tar >nul 2>nul
if %errorlevel% neq 0 (
    echo [ERROR] 未找到 tar 命令，请使用 Win10 1803 及以上版本，
    echo         或手动用 7-Zip / WinRAR 解压 .tar.gz 文件。
    echo         下载地址: %RELEASE_URL%
    pause
    exit /b 1
)

rem ==============================
rem 下载 webrtc 各平台
rem ==============================
call :download_webrtc linux
call :download_webrtc jetson
call :download_webrtc jetson-default
call :download_webrtc win

rem ==============================
rem 下载 Video_Codec_SDK
rem ==============================
call :download_sdk

rem ==============================
rem 下载 zjlabs.yuv 测试视频
rem ==============================
call :download_zjlabs_yuv

rem 清理临时目录
rmdir /s /q "%TEMP_DIR%" 2>nul

echo.
echo ==========================================
echo   Done! 所有 third_party 依赖已就绪
echo ==========================================
pause
exit /b 0

rem ==============================
rem 子函数：下载 webrtc
rem ==============================
:download_webrtc
set "PLATFORM=%~1"
set "TARGET=%PROJECT_ROOT%\third_party\webrtc\webrtc-%PLATFORM%"
set "ARCHIVE=webrtc-%PLATFORM%.tar.gz"

if exist "%TARGET%\include" (
    dir /b "%TARGET%\include" >nul 2>nul
    if not errorlevel 1 (
        echo [SKIP] webrtc-%PLATFORM% already exists
        exit /b 0
    )
)

echo [INFO] Downloading %ARCHIVE% ...
curl -L --progress-bar -o "%TEMP_DIR%\%ARCHIVE%" "%RELEASE_URL%/%ARCHIVE%"
if %errorlevel% neq 0 (
    echo [ERROR] 下载失败: %ARCHIVE%
    exit /b 1
)

echo [INFO] Extracting ...
if not exist "%PROJECT_ROOT%\third_party\webrtc" mkdir "%PROJECT_ROOT%\third_party\webrtc"
tar -xzf "%TEMP_DIR%\%ARCHIVE%" -C "%PROJECT_ROOT%\third_party\webrtc"
if %errorlevel% neq 0 (
    echo [ERROR] 解压失败: %ARCHIVE%
    exit /b 1
)

del /q "%TEMP_DIR%\%ARCHIVE%" 2>nul
echo [OK] webrtc-%PLATFORM% done
exit /b 0

rem ==============================
rem 子函数：下载 Video_Codec_SDK
rem ==============================
:download_sdk
set "SDK_DIR=%PROJECT_ROOT%\third_party\Video_Codec_SDK_11.0.10"
set "ARCHIVE=Video_Codec_SDK_11.0.10.tar.gz"

if exist "%SDK_DIR%" (
    dir /b "%SDK_DIR%" >nul 2>nul
    if not errorlevel 1 (
        echo [SKIP] Video_Codec_SDK already exists
        exit /b 0
    )
)

echo [INFO] Downloading %ARCHIVE% ...
curl -L --progress-bar -o "%TEMP_DIR%\%ARCHIVE%" "%RELEASE_URL%/%ARCHIVE%"
if %errorlevel% neq 0 (
    echo [ERROR] 下载失败: %ARCHIVE%
    exit /b 1
)

echo [INFO] Extracting ...
if not exist "%PROJECT_ROOT%\third_party" mkdir "%PROJECT_ROOT%\third_party"
tar -xzf "%TEMP_DIR%\%ARCHIVE%" -C "%PROJECT_ROOT%\third_party"
if %errorlevel% neq 0 (
    echo [ERROR] 解压失败: %ARCHIVE%
    exit /b 1
)

del /q "%TEMP_DIR%\%ARCHIVE%" 2>nul
echo [OK] Video_Codec_SDK done
exit /b 0

rem ==============================
rem 子函数：下载 zjlabs.yuv 测试视频
rem ==============================
:download_zjlabs_yuv
set "YUV_FILE=%PROJECT_ROOT%\test_data\zjlabs.yuv"
set "ARCHIVE=zjlabs.yuv.tar.gz"

if exist "%YUV_FILE%" (
    echo [SKIP] zjlabs.yuv already exists
    exit /b 0
)

echo [INFO] Downloading %ARCHIVE% ...
curl -L --progress-bar -o "%TEMP_DIR%\%ARCHIVE%" "%RELEASE_URL%/%ARCHIVE%"
if %errorlevel% neq 0 (
    echo [ERROR] 下载失败: %ARCHIVE%
    exit /b 1
)

echo [INFO] Extracting ...
if not exist "%PROJECT_ROOT%\test_data" mkdir "%PROJECT_ROOT%\test_data"
tar -xzf "%TEMP_DIR%\%ARCHIVE%" -C "%PROJECT_ROOT%\test_data"
if %errorlevel% neq 0 (
    echo [ERROR] 解压失败: %ARCHIVE%
    exit /b 1
)

del /q "%TEMP_DIR%\%ARCHIVE%" 2>nul
echo [OK] zjlabs.yuv done
exit /b 0
