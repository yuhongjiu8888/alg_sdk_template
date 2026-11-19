#!/bin/bash
# 一键编译 + 打包发布版本
#
# 用法：
#   ./builddeploy.sh                                    # 编译全部目标并打包
#   ./builddeploy.sh aarch64 xmm                        # 仅编译打包 aarch64-xmm
#   ./builddeploy.sh aarch64 svp_acl_cv610              # 仅编译打包 Hi3516CV610
#   ./builddeploy.sh aarch64 svp_acl_dv500              # 仅编译打包 Hi3519DV500
#   ./builddeploy.sh arm ez                            # 仅编译打包 EZ826/JC371
#   ./builddeploy.sh x86_64 mnn                         # 仅编译打包 x86_64-mnn
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

if [[ $# -ne 0 && $# -ne 2 ]]; then
    echo "Usage: $0 [<arch> <backend>]" >&2
    echo "Configure SDK paths in the corresponding CMake files before packaging." >&2
    exit 1
fi

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
#   $1 = arch       (如 arm, aarch64, x86_64)
#   $2 = backend    (如 xmm, mnn, svp_acl_dv500)
#   $3 = is_test    (true=测试包, false=发布包)
#
# 资源目录按芯片分：resources/config/svp_acl/dv500/、resources/model/svp_acl/dv500/，
# 打包时原样保留目录层级及配置内容，运行时按配置约定选择工作目录。
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
    local build_dir="${BUILD_DIR_PREFIX:-build}_linux_${target_id}"

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
        local runner="test_runner"
        [[ "$backend" == "ez" ]] && runner="ez_nv21_runner"
        if [[ -f "${build_dir}/${runner}" ]]; then
            cp "${build_dir}/${runner}" "${pkg_dir}/"
        else
            echo "Warning: ${runner} not found in ${build_dir}"
        fi
    # ---- 发布包：头文件 + 文档 ----
    else
        cp include/alg_interface.h "${pkg_dir}/"
        cp include/alg_types.h     "${pkg_dir}/"
        if [[ -f "alg_sdk_api_documentation_v3.0.0.md" ]]; then
            cp alg_sdk_api_documentation_v3.0.0.md "${pkg_dir}/"
        fi
    fi

    # ---- 配置目录原样拷贝（包含子目录，不解析或改写配置）----
    mkdir -p "${pkg_dir}/resources/config/${res_subdir}"
    cp -a "resources/config/${res_subdir}/." "${pkg_dir}/resources/config/${res_subdir}/"

    if [[ "$backend" == "ez" ]]; then
        if [[ -f "doc/EZ826_INTEGRATION.md" ]]; then
            cp doc/EZ826_INTEGRATION.md "${pkg_dir}/"
        fi
        # Board MDP libraries remain provided by the camera firmware. Include the
        # official NDK runtime alongside our library; LD_LIBRARY_PATH=./lib is
        # also supported on loaders that do not inherit an executable's RPATH.
        local ez_runtime_manifest="${build_dir}/ez_runtime_libraries.txt"
        if [[ ! -s "${ez_runtime_manifest}" ]]; then
            echo "ERROR: missing EZ runtime library list ${ez_runtime_manifest}" >&2
            return 1
        fi
        mkdir -p "${pkg_dir}/lib"
        local ez_runtime_lib
        while IFS= read -r ez_runtime_lib; do
            [[ -z "${ez_runtime_lib}" ]] && continue
            if [[ ! -f "${ez_runtime_lib}" ]]; then
                echo "ERROR: missing EZ runtime library ${ez_runtime_lib}" >&2
                return 1
            fi
            cp -L "${ez_runtime_lib}" "${pkg_dir}/lib/"
        done < "${ez_runtime_manifest}"
    fi

    # ---- 模型目录原样拷贝（不查找、筛选或校验模型配对）----
    mkdir -p "${pkg_dir}/resources/model/${res_subdir}"
    cp -a "resources/model/${res_subdir}/." "${pkg_dir}/resources/model/${res_subdir}/"

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
    local backend="$2"
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
