@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion

rem =============================================================================
rem  third_party downloader (optimized version)
rem =============================================================================

rem ==============================
rem  解决 curl 代理污染（关键修复）
rem ==============================
set HTTP_PROXY=
set HTTPS_PROXY=
set ALL_PROXY=
<<<<<<< HEAD
set http_proxy=
set https_proxy=
=======
set http_proxy=http://127.0.0.1:7897
set https_proxy=http://127.0.0.1:7897
>>>>>>> 5c8f59e (新加双目和环路切换)
set all_proxy=

rem ==============================
rem 路径初始化（安全版本）
rem ==============================
set "SCRIPT_DIR=%~dp0"
for %%i in ("%SCRIPT_DIR%..") do set "PROJECT_ROOT=%%~fi"

rem ==============================
rem Release 配置
rem ==============================
set "VERSION=v1.0.0"
set "RELEASE_URL=https://github.com/kunkundi/rtc-framework/releases/download/%VERSION%"

rem ==============================
rem 临时目录
rem ==============================
set "TEMP_DIR=%TEMP%\rtc_thirdparty"
if not exist "%TEMP_DIR%" mkdir "%TEMP_DIR%"

echo ==========================================
echo   Third-party downloader (Windows)
echo ==========================================
echo Release: %RELEASE_URL%
echo.

rem ==============================
rem tar 检查
rem ==============================
where tar >nul 2>nul
if errorlevel 1 (
    echo [ERROR] tar not found. Install Windows 10 1803+ or 7-Zip.
    pause
    exit /b 1
)

rem ==============================
rem download flow
rem ==============================
call :download_webrtc linux
if errorlevel 1 goto :failed

call :download_webrtc jetson
if errorlevel 1 goto :failed

call :download_webrtc jetson-default
if errorlevel 1 goto :failed

call :download_webrtc win
if errorlevel 1 goto :failed

call :download_sdk
if errorlevel 1 goto :failed

call :download_zjlabs_yuv
if errorlevel 1 goto :failed

rem ==============================
rem cleanup
rem ==============================
rmdir /s /q "%TEMP_DIR%" 2>nul

echo.
echo ==========================================
echo   DONE: all dependencies ready
echo ==========================================
pause
exit /b 0


:failed
echo.
echo ==========================================
echo   FAILED: download interrupted
echo ==========================================
pause
exit /b 1


rem =============================================================================
rem  function: download_webrtc
rem =============================================================================
:download_webrtc
set "PLATFORM=%~1"
set "ARCHIVE=webrtc-%PLATFORM%.tar.gz"
set "TARGET=%PROJECT_ROOT%\third_party\webrtc\webrtc-%PLATFORM%"
set "MARKER=%TARGET%\.download_done"

if exist "%MARKER%" (
    echo [SKIP] webrtc-%PLATFORM% already installed
    exit /b 0
)

echo [INFO] Downloading %ARCHIVE% ...

call :curl_download "%RELEASE_URL%/%ARCHIVE%" "%TEMP_DIR%\%ARCHIVE%"
if errorlevel 1 exit /b 1

echo [INFO] Extracting %ARCHIVE% ...

if not exist "%PROJECT_ROOT%\third_party\webrtc" mkdir "%PROJECT_ROOT%\third_party\webrtc"

tar -xzf "%TEMP_DIR%\%ARCHIVE%" -C "%PROJECT_ROOT%\third_party\webrtc"
if errorlevel 1 (
    echo [ERROR] extract failed: %ARCHIVE%
    exit /b 1
)

del /q "%TEMP_DIR%\%ARCHIVE%" 2>nul
echo done > "%MARKER%"

echo [OK] webrtc-%PLATFORM%
exit /b 0


rem =============================================================================
rem  function: Video Codec SDK
rem =============================================================================
:download_sdk
set "ARCHIVE=Video_Codec_SDK_11.0.10.tar.gz"
set "SDK_DIR=%PROJECT_ROOT%\third_party\Video_Codec_SDK_11.0.10"

if exist "%SDK_DIR%" (
    echo [SKIP] Video_Codec_SDK exists
    exit /b 0
)

echo [INFO] Downloading SDK ...

call :curl_download "%RELEASE_URL%/%ARCHIVE%" "%TEMP_DIR%\%ARCHIVE%"
if errorlevel 1 exit /b 1

if not exist "%PROJECT_ROOT%\third_party" mkdir "%PROJECT_ROOT%\third_party"

tar -xzf "%TEMP_DIR%\%ARCHIVE%" -C "%PROJECT_ROOT%\third_party"
if errorlevel 1 (
    echo [ERROR] SDK extract failed
    exit /b 1
)

del /q "%TEMP_DIR%\%ARCHIVE%" 2>nul
echo [OK] SDK done
exit /b 0


rem =============================================================================
rem  function: zjlabs.yuv
rem =============================================================================
:download_zjlabs_yuv
set "ARCHIVE=zjlabs.yuv.tar.gz"
set "TARGET_FILE=%PROJECT_ROOT%\test_data\zjlabs.yuv"

if exist "%TARGET_FILE%" (
    echo [SKIP] zjlabs.yuv exists
    exit /b 0
)

echo [INFO] Downloading test video ...

call :curl_download "%RELEASE_URL%/%ARCHIVE%" "%TEMP_DIR%\%ARCHIVE%"
if errorlevel 1 exit /b 1

if not exist "%PROJECT_ROOT%\test_data" mkdir "%PROJECT_ROOT%\test_data"

tar -xzf "%TEMP_DIR%\%ARCHIVE%" -C "%PROJECT_ROOT%\test_data"
if errorlevel 1 (
    echo [ERROR] yuv extract failed
    exit /b 1
)

del /q "%TEMP_DIR%\%ARCHIVE%" 2>nul
echo [OK] zjlabs.yuv done
exit /b 0


rem =============================================================================
rem  curl wrapper (retry + proxy-safe)
rem =============================================================================
:curl_download
set "URL=%~1"
set "OUT=%~2"

curl -L ^
  --retry 3 ^
  --retry-delay 2 ^
  --connect-timeout 10 ^
  --progress-bar ^
<<<<<<< HEAD
  --noproxy "*" ^
=======
  --noproxy "https://127.0.0.1:7897" ^
>>>>>>> 5c8f59e (新加双目和环路切换)
  -o "%OUT%" "%URL%"

if errorlevel 1 (
    echo [ERROR] curl failed: %URL%
    exit /b 1
)

exit /b 0