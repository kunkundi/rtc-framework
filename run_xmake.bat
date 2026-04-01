@echo off
setlocal

set BUILD_DIR=%cd%\build
set INSTALL_DIR=%cd%\install

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if not exist "%INSTALL_DIR%" mkdir "%INSTALL_DIR%"

rem Install xmake dependencies declared in xmake.lua (including Asio)
xmake require -y || goto :error

xmake f -vy -m debug --builddir="%BUILD_DIR%" || goto :error
xmake -j 12 || goto :error
xmake install -o "%INSTALL_DIR%" || goto :error

@REM xmake f -vy -m release --builddir="%BUILD_DIR%" || goto :error
@REM xmake -j 12 || goto :error
@REM xmake install -o "%INSTALL_DIR%" || goto :error

endlocal
exit /b 0

:error
set ERR=%ERRORLEVEL%
endlocal & exit /b %ERR%
