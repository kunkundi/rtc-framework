#! /bin/sh
SOURCE_DIR=$(cd `dirname $0`; pwd)
CMAKE_BUILD_DIR=$SOURCE_DIR/build
CMAKE_INSTALL_DIR=$SOURCE_DIR/install
CMAKE_GENERATOR="Unix Makefiles"

mkdir -p $CMAKE_BUILD_DIR
mkdir -p $CMAKE_INSTALL_DIR

cd $CMAKE_BUILD_DIR

cmake -G "$CMAKE_GENERATOR" \
    -DQTDIR="/home/zhujian/Qt5.12.7/5.12.7/gcc_64" \
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$CMAKE_BUILD_DIR/runtime" \
    -DCMAKE_INSTALL_PREFIX="$CMAKE_INSTALL_DIR" \
    "$SOURCE_DIR"

cmake --build . --config Release
cmake --install . --config Release
