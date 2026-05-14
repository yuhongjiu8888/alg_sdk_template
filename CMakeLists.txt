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

if(NOT DEFINED ALG_BACKEND)
    set(ALG_BACKEND "xmm" CACHE STRING "Inference backend: xmm | rk | mnn")
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
