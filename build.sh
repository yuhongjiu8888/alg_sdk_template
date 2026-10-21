#!/bin/bash
# Build the algorithm SDK for a given platform + chip backend.
#
# Examples:
#   ./build.sh linux aarch64 xmm           # XMM chip on aarch64 Linux (cross-compile)
#   ./build.sh linux aarch64 svp_acl_cv610 # HiSilicon Hi3516CV610 (32-bit arm-musleabi)
#   ./build.sh linux aarch64 svp_acl_dv500 # HiSilicon Hi3519DV500 (aarch64)
#   ./build.sh linux aarch64 rk            # Rockchip backend on aarch64 Linux
#   ./build.sh linux x86_64 mnn            # MNN backend for local x86_64 CPU inference
#
# `svp_acl` is still accepted as an alias of `svp_acl_cv610`.
#

set -e

: ${BUILD_JOBS:=8}
: ${BUILD_DIR_PREFIX:=build}

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "${REPO_ROOT}"

# shellcheck source=targets.sh
source "${REPO_ROOT}/targets.sh"

show_usage() {
    cat <<EOF
Usage: $0 <platform> <arch> <backend>
  platform = linux
  arch     = aarch64 | x86_64
  backend  = xmm | svp_acl_cv610 | svp_acl_dv500 | rk (aarch64) | mnn (x86_64)

For mnn backend, pass -DMNN_ROOT=/path/to/MNN to override the default
MNN install path (/root/opensource/mnn).

HiSilicon toolchain / SDK paths can be overridden through CMake, e.g.
-DSVP_ACL_ROOT=... -DSVP_OPENCV_ROOT=... -DHISI_SECUREC_LIB_DIR=...
EOF
    exit 1
}

[ $# -ne 3 ] && show_usage
platform=$1
arch=$2
backend=$(alg_normalize_backend "$3")

if [ "$backend" != "$3" ]; then
    echo "note: backend '$3' is an alias of '$backend'"
fi

build_dir="${BUILD_DIR_PREFIX}_${platform}_${arch}_${backend}"
rm -rf "${build_dir}"
mkdir -p "${build_dir}"

case "${platform}-${arch}" in
    linux-aarch64)
        _tc_rel=$(alg_toolchain_file "${backend}")
        cd "${build_dir}"
        toolchain="${REPO_ROOT}/${_tc_rel#./}"
        cmake \
            -DCMAKE_TOOLCHAIN_FILE="${toolchain}" \
            -DCMAKE_BUILD_TYPE=Release \
            -DLINUX_AARCH64=ON \
            -DALG_BACKEND="${backend}" \
            -DALG_LOG_INFO=OFF \
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
