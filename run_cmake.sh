#! /bin/sh
set -e

SOURCE_DIR=$(cd `dirname $0`; pwd)
CMAKE_BUILD_DIR=$SOURCE_DIR/build
CMAKE_INSTALL_DIR=$SOURCE_DIR/install
CMAKE_GENERATOR="Unix Makefiles"

if [ -z "${ASIO_PATH}" ] \
    && [ ! -f /usr/include/asio.hpp ] \
    && [ ! -f /usr/local/include/asio.hpp ] \
    && [ ! -f /usr/include/asio/asio.hpp ] \
    && [ ! -f /usr/local/include/asio/asio.hpp ]; then
    echo "Standalone Asio not found."
    echo "Please install it first: sudo apt-get install libasio-dev"
    echo "Or set ASIO_PATH=/path/to/asio/include and rerun this script."
    exit 1
fi

mkdir -p $CMAKE_BUILD_DIR
mkdir -p $CMAKE_INSTALL_DIR

cd $CMAKE_BUILD_DIR

export CC=/usr/bin/clang
export CXX=/usr/bin/clang++

cmake -G "$CMAKE_GENERATOR" \
    -DCMAKE_CXX_FLAGS=-stdlib=libstdc++ \
    -DCMAKE_EXE_LINKER_FLAGS=-stdlib=libstdc++ \
    ${ASIO_PATH:+-DASIO_PATH=$ASIO_PATH} \
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$CMAKE_BUILD_DIR/runtime" \
    -DCMAKE_INSTALL_PREFIX="$CMAKE_INSTALL_DIR" \
    "$SOURCE_DIR"

cmake --build . --config Release --parallel 8
cmake --install . --config Release
