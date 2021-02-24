setlocal

set CMAKE_BUILD_DIR=%cd%\build

if not exist "%CMAKE_BUILD_DIR%" mkdir "%CMAKE_BUILD_DIR%"

pushd "%CMAKE_BUILD_DIR%" && ^
cmake -G "Visual Studio 16 2019" -A x64 ^
    -DQTDIR="D:\software\Qt5.12.7\5.12.7\msvc2017_64" ^
    -DBOOST_ROOT="D:\software\boost_1_73_0" ^
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="%CMAKE_BUILD_DIR%\runtime" ^
    ..
popd

endlocal
