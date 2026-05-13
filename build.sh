#!/bin/bash
# Build the algorithm SDK for a given platform + chip backend.
#
# Examples:
#   ./build.sh linux aarch64 xmm    # XMM chip on aarch64 Linux
#   ./build.sh linux aarch64 rk     # Rockchip backend on aarch64 Linux
#

set -e

: ${BUILD_JOBS:=8}
: ${BUILD_DIR_PREFIX:=build}

show_usage() {
    cat <<EOF
Usage: $0 <platform> <arch> <backend>
  platform = linux
  arch     = aarch64
  backend  = xmm | rk
EOF
    exit 1
}

[ $# -ne 3 ] && show_usage
platform=$1
arch=$2
backend=$3

case "${platform}-${arch}" in
    linux-aarch64)
        build_dir="${BUILD_DIR_PREFIX}_${platform}_${arch}_${backend}"
        rm -rf "${build_dir}"
        mkdir -p "${build_dir}"
        cd "${build_dir}"
        cmake \
            -DCMAKE_TOOLCHAIN_FILE=./toolchain/sgk_linux.toolchain.cmake \
            -DCMAKE_BUILD_TYPE=Release \
            -DLINUX_AARCH64=ON \
            -DALG_BACKEND="${backend}" \
            ..
        make -j${BUILD_JOBS}
        cd ..
        ;;
    *)
        echo "Unsupported platform-arch: ${platform}-${arch}" >&2
        show_usage
        ;;
esac

echo "Build OK: ${platform}-${arch}-${backend}"
