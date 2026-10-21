set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(HISI_TOOLCHAIN_ROOT
    "/home/yuwei/opt/gcc-20240819-aarch64-v01c01-linux-gnu/aarch64-v01c01-linux-gnu-gcc"
    CACHE PATH "HiSilicon DV500 cross compiler root")

set(CMAKE_C_COMPILER   "${HISI_TOOLCHAIN_ROOT}/bin/aarch64-linux-gnu-gcc")
set(CMAKE_CXX_COMPILER "${HISI_TOOLCHAIN_ROOT}/bin/aarch64-linux-gnu-g++")
set(CMAKE_AR           "${HISI_TOOLCHAIN_ROOT}/bin/aarch64-linux-gnu-ar")
set(CMAKE_RANLIB       "${HISI_TOOLCHAIN_ROOT}/bin/aarch64-linux-gnu-gcc-ranlib")
set(CMAKE_STRIP        "${HISI_TOOLCHAIN_ROOT}/bin/aarch64-linux-gnu-strip")
set(CMAKE_OBJCOPY      "${HISI_TOOLCHAIN_ROOT}/bin/aarch64-linux-gnu-objcopy")
set(CMAKE_OBJDUMP      "${HISI_TOOLCHAIN_ROOT}/bin/aarch64-linux-gnu-objdump")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
