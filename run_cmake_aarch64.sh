#! /bin/sh
SOURCE_DIR=$(cd `dirname $0`; pwd)
CMAKE_BUILD_DIR=$SOURCE_DIR/build
CMAKE_INSTALL_DIR=$SOURCE_DIR/install
CMAKE_GENERATOR="Unix Makefiles"

mkdir -p $CMAKE_BUILD_DIR
mkdir -p $CMAKE_INSTALL_DIR

cd $CMAKE_BUILD_DIR

export CC=/usr/bin/clang
export CXX=/usr/bin/clang++

cmake -G "$CMAKE_GENERATOR" \
    -DUSE_DEFAULT_JETSON_ENCODER=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS=-stdlib=libstdc++ \
    -DCMAKE_EXE_LINKER_FLAGS=-stdlib=libstdc++ \
    -DQTDIR="/usr/lib/aarch64-linux-gnu/cmake/Qt5" \
    -DBoost_USE_STATIC_LIBS=ON \
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$CMAKE_BUILD_DIR/runtime" \
    -DCMAKE_INSTALL_PREFIX="$CMAKE_INSTALL_DIR" \
    "$SOURCE_DIR"

cmake --build . --parallel 8
cmake --install . 
