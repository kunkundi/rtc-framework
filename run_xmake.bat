@echo off
setlocal

set BUILD_DIR=%cd%\build
set INSTALL_DIR=%cd%\install

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if not exist "%INSTALL_DIR%" mkdir "%INSTALL_DIR%"

xmake f -y -m debug --builddir="%BUILD_DIR%"
xmake -j 12
xmake install -o "%INSTALL_DIR%"

xmake f -y -m release --builddir="%BUILD_DIR%"
xmake -j 12
xmake install -o "%INSTALL_DIR%"

endlocal
