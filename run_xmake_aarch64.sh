#! /bin/sh
set -e

SOURCE_DIR=$(cd "$(dirname "$0")"; pwd)
BUILD_DIR="$SOURCE_DIR/build"
INSTALL_DIR="$SOURCE_DIR/install"

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

mkdir -p "$BUILD_DIR"
mkdir -p "$INSTALL_DIR"

cd "$SOURCE_DIR"

if [ -n "${QT_DIR}" ] && [ -d "${QT_DIR}/bin" ]; then
    export PATH="${QT_DIR}/bin:${PATH}"
fi

xmake f \
    -m release \
    --builddir="$BUILD_DIR" \
    --use_default_jetson_encoder=n \
    --enable_encode_perf_stats=n \
    ${ASIO_PATH:+--asio_path="$ASIO_PATH"}

xmake -j 8
xmake install -o "$INSTALL_DIR"
