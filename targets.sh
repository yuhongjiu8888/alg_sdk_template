#!/bin/bash
# backend token -> 工具链 / 资源目录的唯一映射表。
#
# build.sh 与 builddeploy.sh 都 source 本文件，避免两处各写一份 case 而漂移。
# 本文件只定义函数，不产生副作用，可安全 source。
#
# 后端取值：
#   xmm           XMM 芯片（sgk 工具链）
#   svp_acl_cv610 海思 Hi3516CV610，32 位 arm-musleabi
#   svp_acl_dv500 海思 Hi3519DV500，aarch64
#   rk            瑞芯微（接口桩）
#   mnn           x86_64 本地推理
# 旧值 svp_acl 等价于 svp_acl_cv610。

# 把历史写法归一化成当前 token。
alg_normalize_backend() {
    case "$1" in
        svp_acl) echo "svp_acl_cv610" ;;
        *)       echo "$1" ;;
    esac
}

# backend token -> 交叉编译工具链文件（相对仓库根目录）。
alg_toolchain_file() {
    case "$1" in
        svp_acl_cv610) echo "./toolchain/hisi_v610_linux.toolchain.cmake" ;;
        svp_acl_dv500) echo "./toolchain/hisi_d500_linux.toolchain.cmake" ;;
        *)             echo "./toolchain/sgk_linux.toolchain.cmake" ;;
    esac
}

# backend token -> resources/config 与 resources/model 下共用的芯片子路径。
# 非海思后端退化为 backend 名本身（保持原有 resources/<kind>/<backend>/ 布局）。
alg_resource_subdir() {
    case "$1" in
        svp_acl_cv610) echo "svp_acl/cv610" ;;
        svp_acl_dv500) echo "svp_acl/dv500" ;;
        *)             echo "$1" ;;
    esac
}
