#!/usr/bin/env bash
# 构建 pyaxvideo whl。
#   ./scripts/build_wheels.sh x86       # linux_x86_64,内含 axcl 后端
#   ./scripts/build_wheels.sh aarch64   # linux_aarch64,内含 axcl + ax650 双后端
# aarch64 需要环境变量:
#   TOOLCHAIN_FILE  aarch64 交叉工具链 cmake 文件
#   MSP_DIR         AX650 MSP 根目录(msp/)
#   AXCL_ARM64_DIR  AXCL aarch64 SDK 根目录
set -euo pipefail
cd "$(dirname "$0")/.."
SDK=..
NATIVE=src/pyaxvideo/_native
rm -f "$NATIVE"/*.so

build_capi() { # <build_dir> <extra cmake args...>
    local dir=$1; shift
    cmake -S "$SDK" -B "$SDK/$dir" -DCMAKE_BUILD_TYPE=Release \
        -DAXSDK_BUILD_SHARED=OFF -DAXSDK_BUILD_CAPI=ON "$@" > /dev/null
    cmake --build "$SDK/$dir" --target axvideo_capi -j"$(nproc)" | tail -1
}

case "${1:-x86}" in
x86)
    build_capi build_capi_x86 -DAXSDK_CHIP_TYPE=axcl ${AXCL_X86_DIR:+-DAXSDK_AXCL_DIR=$AXCL_X86_DIR}
    cp "$SDK"/build_capi_x86/libaxvideo_capi_axcl.so "$NATIVE"/
    PLAT=linux_x86_64
    ;;
aarch64)
    : "${TOOLCHAIN_FILE:?}" "${MSP_DIR:?}" "${AXCL_ARM64_DIR:?}"
    build_capi build_capi_arm64_axcl -DAXSDK_CHIP_TYPE=axcl \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" -DAXSDK_AXCL_DIR="$AXCL_ARM64_DIR"
    build_capi build_capi_arm64_650 -DAXSDK_CHIP_TYPE=ax650 \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" -DAXSDK_MSP_DIR="$MSP_DIR"
    cp "$SDK"/build_capi_arm64_axcl/libaxvideo_capi_axcl.so "$NATIVE"/
    cp "$SDK"/build_capi_arm64_650/libaxvideo_capi_ax650.so "$NATIVE"/
    PLAT=linux_aarch64
    ;;
*)
    echo "usage: $0 [x86|aarch64]" >&2; exit 2
    ;;
esac

rm -rf build ./*.egg-info
PYAXV_PLAT=$PLAT python3 setup.py -q bdist_wheel
ls -la dist/
