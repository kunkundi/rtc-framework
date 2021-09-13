setlocal

set CMAKE_BUILD_DIR=%cd%\build_msvc142
set CMAKE_INSTALL_DIR=%cd%\install_msvc142

if not exist "%CMAKE_BUILD_DIR%" mkdir "%CMAKE_BUILD_DIR%"
if not exist "%CMAKE_INSTALL_DIR%" mkdir "%CMAKE_INSTALL_DIR%"

pushd "%CMAKE_BUILD_DIR%" && ^
cmake -G "Visual Studio 16 2019" -A x64 ^
    -DCMAKE_GENERATOR_TOOLSET=ClangCL ^
    -DQTDIR="D:\software\Qt5.12.7\5.12.7\msvc2017_64" ^
    -DBOOST_ROOT="D:\software\boost_1_73_0" ^
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG="%CMAKE_BUILD_DIR%\runtime\Debug" ^
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE="%CMAKE_BUILD_DIR%\runtime\Release" ^
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO="%CMAKE_BUILD_DIR%\runtime\Release" ^
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL="%CMAKE_BUILD_DIR%\runtime\Release" ^
    -DCMAKE_INSTALL_PREFIX="%CMAKE_INSTALL_DIR%" ^
    ..

cmake --build . --config Debug
cmake --install . --config Debug
cmake --build . --config Release
cmake --install . --config Release
popd

endlocal
