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

# 日志开关（默认全关，零热路径开销）。排查时打开：
#   cmake .. -DALG_LOG_DEBUG=ON   # 板端诊断 dump（xmm/in/out/det/cls-diag）
#   cmake .. -DALG_LOG_INFO=ON    # 每帧耗时等 info
option(ALG_LOG_INFO  "Enable ALG_LOGI per-frame info logs"        OFF)
option(ALG_LOG_DEBUG "Enable ALG_LOGD board debug diagnostics"    OFF)
if(ALG_LOG_INFO)
    add_compile_definitions(ALG_ENABLE_LOG_INFO)
    message(STATUS "ALG_LOGI (info) enabled")
endif()
if(ALG_LOG_DEBUG)
    add_compile_definitions(ALG_ENABLE_LOG_DEBUG)
    message(STATUS "ALG_LOGD (debug diag) enabled")
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

if(NOT DEFINED ALG_BACKEND)
    set(ALG_BACKEND "xmm" CACHE STRING
        "Inference backend: xmm | svp_acl | rk | mnn")
endif()
message(STATUS "ALG_BACKEND = ${ALG_BACKEND}")

if(LINUX_AARCH64)
    message(STATUS "Loading Linux aarch64 configuration")
    include(cmake/CMakeLists_linux_aarch64.cmake)
elseif(LINUX_X86_64)
    message(STATUS "Loading Linux x86_64 configuration")
    include(cmake/CMakeLists_linux_x86_64.cmake)
else()
    message(FATAL_ERROR "No platform selected. Pass -DLINUX_AARCH64=ON or -DLINUX_X86_64=ON.")
endif()
