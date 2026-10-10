#!/bin/bash
# lucky-page 构建脚本（仅 ESP32-S3；基线：page-player/scripts/build.sh 裁剪）
# 自带 IDF 环境导出，无需 activate 脚本。
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# ESP-IDF v6.1.0 环境（工具版本组合与 ~/.espressif/v6.1 匹配：GCC 15.2 / CMake 4.0.3 / Ninja 1.12.1）
export IDF_TOOLS_PATH="$HOME/.espressif/tools"
export IDF_PATH="$HOME/.espressif/v6.1/esp-idf"
export IDF_PYTHON_ENV_PATH="$HOME/.espressif/tools/python/v6.1/venv"
# v6.1 组件管理器要求该变量（官方 activate.py 会设置；手工导出环境须自备，否则 idf.py 启动即崩溃）
export ESP_IDF_VERSION="6.1"
export ESP_ROM_ELF_DIR="$IDF_TOOLS_PATH/esp-rom-elfs/20241011"
export PATH="$IDF_TOOLS_PATH/cmake/4.0.3/CMake.app/Contents/bin:$IDF_TOOLS_PATH/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin:$IDF_TOOLS_PATH/ninja/1.12.1:$IDF_TOOLS_PATH/openocd-esp32/v0.12.0-esp32-20260703/openocd-esp32/bin:$IDF_PYTHON_ENV_PATH/bin:$IDF_PATH/tools:$PATH"

TARGET="esp32s3"
ACTION="build"
PORT=""
EXTRA_ARGS=()

while [[ $# -gt 0 ]]; do
    case $1 in
        s3|esp32s3) shift ;;
        build) shift ;;
        flash) ACTION="build flash"; shift ;;
        # 只烧 app 分区：**不会重写 SPIFFS 资源分区**，因此用户上传的 logo 等文件得以保留。
        # 开发时改了固件请优先用 appflash（用 flash 会把上传的 logo 冲掉 —— 真机踩坑）
        # 注意：必须连分区表一起烧回！只烧 app 而分区表停留在"测试宿主"版本时，
        # 主固件会找不到 assets 分区（真机事故：无限重启）
        appflash) ACTION="build partition-table-flash app-flash"; shift ;;
        merge-bin) ACTION="merge-bin"; shift ;;
        monitor) ACTION="monitor"; shift ;;
        clean) ACTION="fullclean"; shift ;;
        -p|--port) PORT="-p $2"; shift 2 ;;
        *) EXTRA_ARGS+=("$1"); shift ;;
    esac
done

BUILD_DIR="${PROJECT_DIR}/build-esp32s3"
OUTPUT_DIR="${PROJECT_DIR}/output"

if [[ "$ACTION" == "fullclean" ]]; then
    rm -rf "${BUILD_DIR}"
    echo "已清除 ${BUILD_DIR}/"
    exit 0
fi

cd "$PROJECT_DIR"

if [[ "$ACTION" == "monitor" ]]; then
    idf.py -B "${BUILD_DIR}" $PORT monitor "${EXTRA_ARGS[@]}"
elif [[ "$ACTION" == "merge-bin" ]]; then
    VERSION=$(grep -m1 'project(lucky-page VERSION' CMakeLists.txt | sed 's/.*VERSION \([0-9.]*\)).*/\1/')
    TIMESTAMP=$(date +%Y%m%d%H%M%S)
    mkdir -p "${OUTPUT_DIR}"
    OUTPUT="${EXTRA_ARGS[0]:-lucky-page-esp32s3-${VERSION}-${TIMESTAMP}.bin}"
    idf.py -B "${BUILD_DIR}" merge-bin --output "${OUTPUT_DIR}/${OUTPUT}"
    echo "固件已合并: ${OUTPUT_DIR}/${OUTPUT}"
else
    if [[ ! -f "${BUILD_DIR}/build.ninja" ]]; then
        echo "首次构建 ${BUILD_DIR}/（可能需要联网拉取组件元数据）..."
        IDF_COMPONENT_CHECK_NEWER_VERSIONS=0 idf.py -B "${BUILD_DIR}" set-target "${TARGET}"
        IDF_COMPONENT_CHECK_NEWER_VERSIONS=0 idf.py -B "${BUILD_DIR}" $ACTION $PORT "${EXTRA_ARGS[@]}"
    else
        echo "增量构建: ${BUILD_DIR}/"
        IDF_COMPONENT_CHECK_NEWER_VERSIONS=0 idf.py -B "${BUILD_DIR}" $ACTION $PORT "${EXTRA_ARGS[@]}"
    fi
fi
