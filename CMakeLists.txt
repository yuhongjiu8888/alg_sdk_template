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

# 默认运行期日志等级为 Warn，可用 AlgSetLogLevel 动态调整。以下构建选项仅修改
# 启动默认等级，所有日志等级都会编入：
#   cmake .. -DALG_LOG_DEBUG=ON   # 默认 Debug
#   cmake .. -DALG_LOG_INFO=ON    # 默认 Info
option(ALG_LOG_INFO  "Use Info as the default runtime log level"  OFF)
option(ALG_LOG_DEBUG "Use Debug as the default runtime log level" OFF)
option(ALG_SVP_DYNAMIC_AIPP "Enable HiSilicon dynamic AIPP integration" ON)
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

if(NOT DEFINED ALG_BACKEND)
    set(ALG_BACKEND "xmm" CACHE STRING
        "Inference backend: xmm | svp_acl | rk | mnn")
endif()
message(STATUS "ALG_BACKEND = ${ALG_BACKEND}")
if(ALG_BACKEND STREQUAL "svp_acl" AND ALG_SVP_DYNAMIC_AIPP)
    add_compile_definitions(ALG_SVP_DYNAMIC_AIPP=1)
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
