#!/bin/bash
# Build the algorithm SDK for a given platform + chip backend.
#
# Examples:
#   ./build.sh linux aarch64 xmm    # XMM chip on aarch64 Linux (cross-compile)
#   ./build.sh linux aarch64 svp_acl # HiSilicon SVP ACL on v610
#   ./build.sh linux aarch64 rk      # Rockchip backend on aarch64 Linux
#   ./build.sh linux x86_64 mnn     # MNN backend for local x86_64 CPU inference
#

set -e

: ${BUILD_JOBS:=8}
: ${BUILD_DIR_PREFIX:=build}

show_usage() {
    cat <<EOF
Usage: $0 <platform> <arch> <backend>
  platform = linux
  arch     = aarch64 | x86_64
  backend  = xmm | svp_acl | rk (aarch64) | mnn (x86_64)

For mnn backend, pass -DMNN_ROOT=/path/to/MNN to override the default
MNN install path (/root/opensource/mnn).
EOF
    exit 1
}

[ $# -ne 3 ] && show_usage
platform=$1
arch=$2
backend=$3

build_dir="${BUILD_DIR_PREFIX}_${platform}_${arch}_${backend}"
rm -rf "${build_dir}"
mkdir -p "${build_dir}"

case "${platform}-${arch}" in
    linux-aarch64)
        cd "${build_dir}"
        if [[ "${backend}" == "svp_acl" ]]; then
            toolchain="./toolchain/hisi_v610_linux.toolchain.cmake"
        else
            toolchain="./toolchain/sgk_linux.toolchain.cmake"
        fi
        cmake \
            -DCMAKE_TOOLCHAIN_FILE="${toolchain}" \
            -DCMAKE_BUILD_TYPE=Release \
            -DLINUX_AARCH64=ON \
            -DALG_BACKEND="${backend}" \
            -DALG_LOG_INFO=ON \
            ..
        make -j${BUILD_JOBS}
        cd ..
        ;;
    linux-x86_64)
        cd "${build_dir}"
        cmake \
            -DCMAKE_BUILD_TYPE=Release \
            -DLINUX_X86_64=ON \
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
