#!/bin/bash
# 一键编译 + 打包发布版本
#
# 用法：
#   ./builddeploy.sh                                    # 编译全部目标并打包
#   ./builddeploy.sh aarch64 xmm                        # 仅编译打包 aarch64-xmm
#   ./builddeploy.sh aarch64 svp_acl_cv610              # 仅编译打包 Hi3516CV610
#   ./builddeploy.sh aarch64 svp_acl_dv500              # 仅编译打包 Hi3519DV500
#   ./builddeploy.sh x86_64 mnn                         # 仅编译打包 x86_64-mnn
#
# （`svp_acl` 仍可用，等价于 `svp_acl_cv610`）
#
# 输出在 deploy/ 下，每个目标两个 zip（后缀带打包时间戳，便于区分新旧）：
#   alg_sdk_<VERSION>_<TARGET>_<YYYYmmdd_HHMMSS>.zip        # SDK 发布包
#   TEST_alg_sdk_<VERSION>_<TARGET>_<YYYYmmdd_HHMMSS>.zip   # 测试包

set -e

# ==============================================================
# 配置
# ==============================================================
: "${BUILD_JOBS:=8}"
DEPLOY_DIR="deploy"
BUILD_SCRIPT="./build.sh"

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "${REPO_ROOT}"

# backend token -> 工具链 / 资源子目录的唯一映射表（与 build.sh 共用）
# shellcheck source=targets.sh
source "${REPO_ROOT}/targets.sh"

# 从源码中提取版本号
function get_sdk_version() {
    grep '#define ALG_VERSION_STRING' src/interface/alg_interface.cpp \
        | head -1 | awk '{print $3}' | tr -d '"'
}

SDK_VERSION=$(get_sdk_version | sed 's/^alg_sdk\.//')
echo "SDK version: ${SDK_VERSION}"

# 本次打包的时间戳（多目标构建共用同一个，便于归档识别）
BUILD_TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
echo "Build timestamp: ${BUILD_TIMESTAMP}"

# ==============================================================
# 校验后端是否有对应的配置/模型目录
# ==============================================================
function check_backend_resources() {
    local backend="$1"
    local res_subdir="$2"
    if [[ ! -d "resources/config/${res_subdir}" ]]; then
        echo "ERROR: backend '${backend}' has no corresponding config directory"
        echo "       resources/config/${res_subdir}/ does not exist"
        exit 1
    fi
    if [[ ! -d "resources/model/${res_subdir}" ]]; then
        echo "ERROR: backend '${backend}' has no corresponding model directory"
        echo "       resources/model/${res_subdir}/ does not exist"
        exit 1
    fi
}

# ==============================================================
# 打包函数
#
# 参数:
#   $1 = arch       (如 aarch64, x86_64)
#   $2 = backend    (如 xmm, mnn, svp_acl_dv500)
#   $3 = is_test    (true=测试包, false=发布包)
#
# 资源目录按芯片分：resources/config/svp_acl/dv500/、resources/model/svp_acl/dv500/，
# 打包时保持同样的相对层级，JSON 里的 ../resources/model/... 才能解析到。
# ==============================================================
function package_artifacts() {
    local arch="$1"
    local backend="$2"
    local is_test="$3"
    local target_id="${arch}_${backend}"
    local res_subdir
    res_subdir=$(alg_resource_subdir "${backend}")

    if [[ "$is_test" == "true" ]]; then
        local folder_name="TEST_alg_sdk_${SDK_VERSION}_${target_id}"
    else
        local folder_name="alg_sdk_${SDK_VERSION}_${target_id}"
    fi

    local pkg_dir="${DEPLOY_DIR}/${folder_name}"
    local build_dir="build_linux_${target_id}"

    echo ""
    echo "--- Packaging ${folder_name} ---"

    mkdir -p "${pkg_dir}"

    # ---- libalg_sdk.so ----
    if [[ -f "${build_dir}/libalg_sdk.so" ]]; then
        cp "${build_dir}/libalg_sdk.so" "${pkg_dir}/"
    else
        echo "Warning: libalg_sdk.so not found in ${build_dir}"
    fi

    # ---- 测试包：test_runner ----
    if [[ "$is_test" == "true" ]]; then
        if [[ -f "${build_dir}/test_runner" ]]; then
            cp "${build_dir}/test_runner" "${pkg_dir}/"
        else
            echo "Warning: test_runner not found in ${build_dir}"
        fi
    # ---- 发布包：头文件 + 文档 ----
    else
        cp include/alg_interface.h "${pkg_dir}/"
        cp include/alg_types.h     "${pkg_dir}/"
        if [[ -f "alg_sdk_api_documentation_v3.0.0.md" ]]; then
            cp alg_sdk_api_documentation_v3.0.0.md "${pkg_dir}/"
        fi
    fi

    # ---- 业务 JSON 配置（speed_limit / license_plate / ...）----
    # 保持 resources/config/<res_subdir>/ 目录结构，与 JSON 内相对路径一致
    mkdir -p "${pkg_dir}/resources/config/${res_subdir}"
    cp resources/config/${res_subdir}/*.json \
       "${pkg_dir}/resources/config/${res_subdir}/" 2>/dev/null || {
        echo "ERROR: no config json under resources/config/${res_subdir}/"
        exit 1
    }
    if [[ -d "resources/config/${res_subdir}/aipp" ]]; then
        cp -R "resources/config/${res_subdir}/aipp" \
              "${pkg_dir}/resources/config/${res_subdir}/"
    fi

    # ---- 模型文件（排除红绿灯模型）----
    mkdir -p "${pkg_dir}/resources/model/${res_subdir}"
    for f in "resources/model/${res_subdir}/"*; do
        name=$(basename "$f")
        [[ "$name" == traffic_light.* ]] && continue
        cp "$f" "${pkg_dir}/resources/model/${res_subdir}/"
    done

    echo "Package ready: ${pkg_dir}/"

    # 压缩（包名带时间戳，目录名保持不变，解压后的目录结构不受影响）
    local zip_name="${folder_name}_${BUILD_TIMESTAMP}.zip"
    cd "${DEPLOY_DIR}"
    zip -r "${zip_name}" "${folder_name}" > /dev/null
    echo "  -> ${DEPLOY_DIR}/${zip_name}  ($(du -sh "${zip_name}" | cut -f1))"
    cd ..
}

# ==============================================================
# 单目标编译 + 打包
# ==============================================================
function build_and_pack() {
    local arch="$1"
    local backend
    backend=$(alg_normalize_backend "$2")
    local target_id="${arch}_${backend}"

    # 编译前先校验后端资源存在
    check_backend_resources "${backend}" "$(alg_resource_subdir "${backend}")"

    echo ""
    echo "=========================================="
    echo "Building ${arch}-${backend} ..."
    echo "=========================================="

    if ! ${BUILD_SCRIPT} linux "${arch}" "${backend}"; then
        echo "ERROR: Build failed for ${arch}-${backend}"
        return 1
    fi

    echo "Build OK: ${arch}-${backend}"
    echo ""

    # 打包发布包 + 测试包
    package_artifacts "${arch}" "${backend}" false
    package_artifacts "${arch}" "${backend}" true
    echo "Package OK: ${target_id}"
}

# ==============================================================
# 主流程
# ==============================================================
ARCH_ARG="$1"
BACKEND_ARG="$2"

if [[ -z "$ARCH_ARG" || -z "$BACKEND_ARG" ]]; then
    echo "No target specified, building all available targets..."
    build_and_pack "aarch64" "xmm"
    build_and_pack "x86_64" "mnn"
else
    build_and_pack "${ARCH_ARG}" "${BACKEND_ARG}"
fi

# ==============================================================
# 汇总
# ==============================================================
echo ""
echo "=========================================="
echo "All builds complete. Packages:"
echo "=========================================="
ls -lh "${DEPLOY_DIR}"/*.zip 2>/dev/null | awk '{print "  " $NF "  (" $5 ")"}'
echo ""
echo "Done. Deploy artifacts in ${DEPLOY_DIR}/"
