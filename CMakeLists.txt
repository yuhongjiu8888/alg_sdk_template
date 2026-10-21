cmake_minimum_required(VERSION 3.10)
project(alg_sdk LANGUAGES C CXX)

#
# Two orthogonal axes:
#   ALG_PLATFORM : how to compile (toolchain, OpenCV, system libs)
#   ALG_BACKEND  : which chip's inferer to link in
#
# Adding a new chip = drop in src/backend/<chip>/ + add an option below.
# Adding a new model = drop in src/models/<model>/ + ModelFactory picks it up.
#
option(LINUX_AARCH64 "Build for aarch64 Linux (cross-compile)" OFF)
option(LINUX_X86_64  "Build for x86_64 Linux (host, local MNN test)" OFF)

# -----------------------------------------------------------------------------
# 后端选择。海思 SVP ACL 有两个芯片变体，由 backend 名字区分：
#   svp_acl_cv610 : Hi3516CV610，32 位 arm-musleabi
#   svp_acl_dv500 : Hi3519DV500，aarch64
# 老写法 svp_acl 仍可用，等价于 svp_acl_cv610。
#
# 由它派生两个内部变量，后续平台配置只用这两个，不再比较 backend 字面量：
#   ALG_SVP_BACKEND : 是否海思 SVP ACL（决定 ACL 头文件、link group 等共性）
#   ALG_SVP_CHIP    : cv610 | dv500（决定工具链、SDK/OpenCV 路径、可用特性）
# -----------------------------------------------------------------------------
if(NOT DEFINED ALG_BACKEND)
    set(ALG_BACKEND "xmm" CACHE STRING
        "Inference backend: xmm | svp_acl_cv610 | svp_acl_dv500 | rk | mnn")
endif()
set(ALG_SVP_BACKEND FALSE)
set(ALG_SVP_CHIP "")
if(ALG_BACKEND MATCHES "^svp_acl")
    set(ALG_SVP_BACKEND TRUE)
    if(ALG_BACKEND STREQUAL "svp_acl_dv500")
        set(ALG_SVP_CHIP "dv500")
    elseif(ALG_BACKEND STREQUAL "svp_acl_cv610" OR ALG_BACKEND STREQUAL "svp_acl")
        set(ALG_SVP_CHIP "cv610")
    else()
        message(FATAL_ERROR
            "Unknown SVP ACL backend '${ALG_BACKEND}' "
            "(expected svp_acl_cv610 or svp_acl_dv500)")
    endif()
endif()

# 默认运行期日志等级为 Warn，可用 AlgSetLogLevel 动态调整。以下构建选项仅修改
# 启动默认等级，所有日志等级都会编入：
#   cmake .. -DALG_LOG_DEBUG=ON   # 默认 Debug
#   cmake .. -DALG_LOG_INFO=ON    # 默认 Info
option(ALG_LOG_INFO  "Use Info as the default runtime log level"  OFF)
option(ALG_LOG_DEBUG "Use Debug as the default runtime log level" OFF)
# 海思前处理开关，默认值按芯片能力取（仍可用 -D 显式覆盖）：
#   DV500 未随 acllib 发布 SVP/IVE（无 ss_mpi_ive.h、libss_mpi_ive.a），
#   因此默认关闭 IVE，只保留连续 NV12/NV21 的 AIPP 零拷贝路径；
#   分平面/无物理地址的输入仍由 libyuv 合并（third_party/libyuv/3519dv500）。
if(ALG_SVP_CHIP STREQUAL "dv500")
    set(_svp_aipp_default  ON)
    set(_svp_ive_default   OFF)
    set(_svp_libyuv_default ON)
else()
    set(_svp_aipp_default  ON)
    set(_svp_ive_default   ON)
    set(_svp_libyuv_default ON)
endif()
option(ALG_SVP_DYNAMIC_AIPP "Enable HiSilicon dynamic AIPP integration" ${_svp_aipp_default})
option(ALG_SVP_IVE_PREPROCESS
       "Use HiSilicon IVE resize for split-plane physical YUV input" ${_svp_ive_default})
option(ALG_SVP_LIBYUV_FALLBACK
       "Use libyuv to merge split-plane input into AIPP staging (no physical address)"
       ${_svp_libyuv_default})
if(ALG_LOG_INFO)
    add_compile_definitions(ALG_ENABLE_LOG_INFO)
    message(STATUS "Default runtime log level: Info")
endif()
if(ALG_LOG_DEBUG)
    add_compile_definitions(ALG_ENABLE_LOG_DEBUG)
    message(STATUS "Default runtime log level: Debug")
endif()
# 异步落盘日志模块（借鉴 spdlog：异步 + 多 sink + 滚动文件）。默认关闭，
# 不影响现有逐字节行为。打开后 ALG_LOGE/W/I/D 改走 core/log/alg_log。
#   cmake .. -DALG_LOG_FILE=ON \
#            -DALG_LOG_FILE_PATH=/data/log/alg.log \
#            -DALG_LOG_FILE_MAX_SIZE=10485760 \   # 单文件上限(字节)，默认 10MB
#            -DALG_LOG_FILE_MAX_FILES=5           # 滚动保留份数(循环覆盖)
option(ALG_LOG_FILE    "Enable async rotating file log module"        OFF)
option(ALG_LOG_ANDROID "Route logs to Android logcat (NDK build)"     OFF)
set(ALG_LOG_FILE_PATH      "alg_sdk.log" CACHE STRING "Log file base path")
set(ALG_LOG_FILE_MAX_SIZE  "10485760"    CACHE STRING "Max bytes per log file before rotation")
set(ALG_LOG_FILE_MAX_FILES "5"           CACHE STRING "Number of rotated log files kept (circular)")
if(ALG_LOG_FILE)
    add_compile_definitions(
        ALG_ENABLE_LOG_FILE
        ALG_LOG_FILE_PATH="${ALG_LOG_FILE_PATH}"
        ALG_LOG_FILE_MAX_SIZE=${ALG_LOG_FILE_MAX_SIZE}
        ALG_LOG_FILE_MAX_FILES=${ALG_LOG_FILE_MAX_FILES}
    )
    message(STATUS "ALG file log enabled: path=${ALG_LOG_FILE_PATH} "
                   "max_size=${ALG_LOG_FILE_MAX_SIZE} max_files=${ALG_LOG_FILE_MAX_FILES}")
endif()
if(ALG_LOG_ANDROID)
    add_compile_definitions(ALG_LOG_ANDROID_DEFAULT=true)
    message(STATUS "ALG android logcat sink enabled")
endif()

message(STATUS "ALG_BACKEND = ${ALG_BACKEND}")
if(ALG_SVP_BACKEND)
    message(STATUS "ALG_SVP_CHIP = ${ALG_SVP_CHIP} "
                   "(aipp=${ALG_SVP_DYNAMIC_AIPP} ive=${ALG_SVP_IVE_PREPROCESS} "
                   "libyuv_fallback=${ALG_SVP_LIBYUV_FALLBACK})")
endif()
if(ALG_SVP_BACKEND)
    add_compile_definitions(ALG_SVP_CHIP="${ALG_SVP_CHIP}")
    if(ALG_SVP_CHIP STREQUAL "dv500")
        # 数值宏，供 C++ 做条件编译（ALG_SVP_CHIP 是字符串，不方便 #ifdef）。
        add_compile_definitions(ALG_SVP_CHIP_DV500=1)
    endif()
    if(ALG_SVP_DYNAMIC_AIPP)
        add_compile_definitions(ALG_SVP_DYNAMIC_AIPP=1)
        if(ALG_SVP_IVE_PREPROCESS)
            add_compile_definitions(ALG_SVP_IVE_PREPROCESS=1)
        endif()
        if(ALG_SVP_LIBYUV_FALLBACK)
            add_compile_definitions(ALG_SVP_LIBYUV_FALLBACK=1)
        endif()
    endif()
endif()

if(LINUX_AARCH64)
    message(STATUS "Loading Linux aarch64 configuration")
    include(cmake/CMakeLists_linux_aarch64.cmake)
elseif(LINUX_X86_64)
    message(STATUS "Loading Linux x86_64 configuration")
    include(cmake/CMakeLists_linux_x86_64.cmake)
else()
    message(FATAL_ERROR "No platform selected. Pass -DLINUX_AARCH64=ON or -DLINUX_X86_64=ON.")
endif()
