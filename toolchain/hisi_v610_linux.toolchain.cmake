set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(HISI_TOOLCHAIN_ROOT
    "/root/project/haisi-v610/gcc-20250305-arm-v01c02-linux-musleabi/arm-v01c02-linux-musleabi-gcc"
    CACHE PATH "HiSilicon v610 cross compiler root")

set(CMAKE_C_COMPILER   "${HISI_TOOLCHAIN_ROOT}/bin/arm-linux-musleabi-gcc")
set(CMAKE_CXX_COMPILER "${HISI_TOOLCHAIN_ROOT}/bin/arm-linux-musleabi-g++")
set(CMAKE_AR           "${HISI_TOOLCHAIN_ROOT}/bin/arm-linux-musleabi-ar")
set(CMAKE_RANLIB       "${HISI_TOOLCHAIN_ROOT}/bin/arm-linux-musleabi-ranlib")
set(CMAKE_STRIP        "${HISI_TOOLCHAIN_ROOT}/bin/arm-linux-musleabi-strip")
set(CMAKE_OBJCOPY      "${HISI_TOOLCHAIN_ROOT}/bin/arm-linux-musleabi-objcopy")
set(CMAKE_OBJDUMP      "${HISI_TOOLCHAIN_ROOT}/bin/arm-linux-musleabi-objdump")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
