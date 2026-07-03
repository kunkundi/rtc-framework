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
# =============================================================================

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# ==============================
# 配置：修改为你的实际下载地址
# ==============================
# 方式一：设置环境变量
#   export RTC_THIRDPARTY_URL="https://github.com/kunkundi/rtc-framework/releases/download/v1.0.0"
#
# 方式二：直接修改这里
RELEASE_URL="${RTC_THIRDPARTY_URL:-}"

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

    if [ -z "$RELEASE_URL" ]; then
        echo "[WARN] RELEASE_URL 未设置，跳过 webrtc-${PLATFORM} 下载"
        echo "       请将 ${ARCHIVE} 解压到: $PROJECT_ROOT/third_party/webrtc/"
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

    if [ -z "$RELEASE_URL" ]; then
        echo "[WARN] RELEASE_URL 未设置，跳过 Video_Codec_SDK 下载"
        echo "       请将 ${ARCHIVE} 解压到: $PROJECT_ROOT/third_party/"
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
# 主流程
# ==============================
main() {
    echo "=========================================="
    echo "  下载 third_party 依赖文件"
    echo "=========================================="
    echo ""

    if [ -n "$RELEASE_URL" ]; then
        echo "Release URL: $RELEASE_URL"
    else
        echo "提示: 设置 RTC_THIRDPARTY_URL 环境变量或修改脚本中的 RELEASE_URL"
    fi
    echo ""

    download_webrtc "linux"
    download_webrtc "jetson"
    download_webrtc "jetson-default"
    download_webrtc "win"
    download_video_codec_sdk

    echo ""
    echo "=========================================="
    echo "  Done! 所有 third_party 依赖已就绪"
    echo "=========================================="
}

main
