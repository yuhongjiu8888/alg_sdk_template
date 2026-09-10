#!/bin/bash
# 一键编译 + 打包发布版本
#
# 用法：
#   ./builddeploy.sh                                    # 编译全部目标并打包
#   ./builddeploy.sh aarch64 xmm                        # 仅编译打包 aarch64-xmm
#   ./builddeploy.sh aarch64 svp_acl                    # 仅编译打包 v610 SVP ACL
#   ./builddeploy.sh x86_64 mnn                         # 仅编译打包 x86_64-mnn
#
# 输出在 deploy/ 下，每个目标两个 zip：
#   alg_sdk_<VERSION>_<TARGET>.zip           # SDK 发布包
#   TEST_alg_sdk_<VERSION>_<TARGET>.zip      # 测试包

set -e

# ==============================================================
# 配置
# ==============================================================
: "${BUILD_JOBS:=8}"
DEPLOY_DIR="deploy"
BUILD_SCRIPT="./build.sh"

# 从源码中提取版本号
function get_sdk_version() {
    grep '#define ALG_VERSION_STRING' src/interface/alg_interface.cpp \
        | head -1 | awk '{print $3}' | tr -d '"'
}

SDK_VERSION=$(get_sdk_version | sed 's/^alg_sdk\.//')
echo "SDK version: ${SDK_VERSION}"

# ==============================================================
# 校验后端是否有对应的配置/模型目录
# ==============================================================
function check_backend_resources() {
    local backend="$1"
    if [[ ! -d "resources/config/${backend}" ]]; then
        echo "ERROR: backend '${backend}' has no corresponding config directory"
        echo "       resources/config/${backend}/ does not exist"
        exit 1
    fi
    if [[ ! -d "resources/model/${backend}" ]]; then
        echo "ERROR: backend '${backend}' has no corresponding model directory"
        echo "       resources/model/${backend}/ does not exist"
        exit 1
    fi
}

# ==============================================================
# 打包函数
#
# 参数:
#   $1 = arch       (如 aarch64, x86_64)
#   $2 = backend    (如 xmm, mnn, rk)
#   $3 = is_test    (true=测试包, false=发布包)
# ==============================================================
function package_artifacts() {
    local arch="$1"
    local backend="$2"
    local is_test="$3"
    local target_id="${arch}_${backend}"

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
    # 保持 resources/config/<backend>/ 目录结构，与 JSON 内相对路径一致
    mkdir -p "${pkg_dir}/resources/config/${backend}"
    cp resources/config/${backend}/*.json \
       "${pkg_dir}/resources/config/${backend}/" 2>/dev/null || {
        echo "ERROR: no config json under resources/config/${backend}/"
        exit 1
    }
    if [[ -d "resources/config/${backend}/aipp" ]]; then
        cp -R "resources/config/${backend}/aipp" \
              "${pkg_dir}/resources/config/${backend}/"
    fi

    # ---- 模型文件（排除红绿灯模型）----
    mkdir -p "${pkg_dir}/resources/model/${backend}"
    for f in "resources/model/${backend}/"*; do
        name=$(basename "$f")
        [[ "$name" == traffic_light.* ]] && continue
        cp "$f" "${pkg_dir}/resources/model/${backend}/"
    done

    echo "Package ready: ${pkg_dir}/"

    # 压缩
    cd "${DEPLOY_DIR}"
    zip -r "${folder_name}.zip" "${folder_name}" > /dev/null
    echo "  -> ${DEPLOY_DIR}/${folder_name}.zip  ($(du -sh "${folder_name}.zip" | cut -f1))"
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
    check_backend_resources "${backend}"

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
