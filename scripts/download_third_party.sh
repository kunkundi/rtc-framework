#!/bin/bash
# =============================================================================
# 下载 third_party 大文件（二进制库）
#
# 使用方法：
#   1. 先在 GitHub Releases 创建 Release 并上传 packages/ 下的 tar.gz 文件
#   2. 修改下方 RELEASE_URL 为实际下载地址
#   3. 运行: bash scripts/download_third_party.sh
#
# Release 中应有的文件:
#   webrtc-linux.tar.gz
#   webrtc-jetson.tar.gz
#   webrtc-jetson-default.tar.gz
#   webrtc-win.tar.gz
#   Video_Codec_SDK_11.0.10.tar.gz
#   zjlabs.yuv.tar.gz
# =============================================================================

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# ==============================
# 配置：固定 Release 下载地址，更新依赖时改版本号即可
# ==============================
RELEASE_URL="https://github.com/kunkundi/rtc-framework/releases/download/v1.0.0"

# ==============================
# 下载 webrtc（include + lib 完整目录）
# ==============================
download_webrtc() {
    local PLATFORM="$1"  # linux | jetson | jetson-default | win
    local TARGET_DIR="$PROJECT_ROOT/third_party/webrtc/webrtc-${PLATFORM}"
    local ARCHIVE="webrtc-${PLATFORM}.tar.gz"

    # 检查是否已存在
    if [ -d "$TARGET_DIR/include" ] && [ "$(ls -A "$TARGET_DIR/include" 2>/dev/null)" != "" ]; then
        echo "[SKIP] webrtc-${PLATFORM} already exists"
        return
    fi

    echo "[INFO] Downloading ${ARCHIVE}..."
    curl -L --progress-bar -o "/tmp/${ARCHIVE}" "${RELEASE_URL}/${ARCHIVE}"
    echo "[INFO] Extracting..."
    mkdir -p "$PROJECT_ROOT/third_party/webrtc"
    tar -xzf "/tmp/${ARCHIVE}" -C "$PROJECT_ROOT/third_party/webrtc/"
    rm -f "/tmp/${ARCHIVE}"
    echo "[OK] webrtc-${PLATFORM} done"
}

# ==============================
# 下载 Video_Codec_SDK
# ==============================
download_video_codec_sdk() {
    local SDK_DIR="$PROJECT_ROOT/third_party/Video_Codec_SDK_11.0.10"
    local ARCHIVE="Video_Codec_SDK_11.0.10.tar.gz"

    if [ -d "$SDK_DIR" ] && [ "$(ls -A "$SDK_DIR" 2>/dev/null)" != "" ]; then
        echo "[SKIP] Video_Codec_SDK already exists"
        return
    fi

    echo "[INFO] Downloading ${ARCHIVE}..."
    curl -L --progress-bar -o "/tmp/${ARCHIVE}" "${RELEASE_URL}/${ARCHIVE}"
    echo "[INFO] Extracting..."
    mkdir -p "$PROJECT_ROOT/third_party"
    tar -xzf "/tmp/${ARCHIVE}" -C "$PROJECT_ROOT/third_party/"
    rm -f "/tmp/${ARCHIVE}"
    echo "[OK] Video_Codec_SDK done"
}

# ==============================
# 下载 zjlabs.yuv 测试视频
# ==============================
download_zjlabs_yuv() {
    local YUV_FILE="$PROJECT_ROOT/test_data/zjlabs.yuv"
    local ARCHIVE="zjlabs.yuv.tar.gz"

    if [ -f "$YUV_FILE" ]; then
        echo "[SKIP] zjlabs.yuv already exists"
        return
    fi

    echo "[INFO] Downloading ${ARCHIVE}..."
    curl -L --progress-bar -o "/tmp/${ARCHIVE}" "${RELEASE_URL}/${ARCHIVE}"
    echo "[INFO] Extracting..."
    mkdir -p "$PROJECT_ROOT/test_data"
    tar -xzf "/tmp/${ARCHIVE}" -C "$PROJECT_ROOT/test_data/"
    rm -f "/tmp/${ARCHIVE}"
    echo "[OK] zjlabs.yuv done"
}

# ==============================
# 主流程
# ==============================
main() {
    echo "=========================================="
    echo "  下载 third_party 依赖文件"
    echo "=========================================="
    echo ""

    echo "Release URL: $RELEASE_URL"
    echo ""

    download_webrtc "linux"
    download_webrtc "jetson"
    download_webrtc "jetson-default"
    download_webrtc "win"
    download_video_codec_sdk
    download_zjlabs_yuv

    echo ""
    echo "=========================================="
    echo "  Done! 所有 third_party 依赖已就绪"
    echo "=========================================="
}

main
