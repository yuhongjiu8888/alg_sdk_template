set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(TOOLCHAIN_DIR /root/project/ez/gcc-linaro-7.5.0-2019.12-x86_64_arm-linux-gnueabihf)

# 编译器
set(CMAKE_C_COMPILER   ${TOOLCHAIN_DIR}/bin/arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_DIR}/bin/arm-linux-gnueabihf-g++)

# sysroot
set(CMAKE_SYSROOT ${TOOLCHAIN_DIR}/arm-linux-gnueabihf/libc)
set(CMAKE_FIND_ROOT_PATH ${CMAKE_SYSROOT})

# binutils
set(CMAKE_AR       ${TOOLCHAIN_DIR}/bin/arm-linux-gnueabihf-ar)
set(CMAKE_RANLIB   ${TOOLCHAIN_DIR}/bin/arm-linux-gnueabihf-ranlib)
set(CMAKE_STRIP    ${TOOLCHAIN_DIR}/bin/arm-linux-gnueabihf-strip)
set(CMAKE_OBJCOPY  ${TOOLCHAIN_DIR}/bin/arm-linux-gnueabihf-objcopy)
set(CMAKE_OBJDUMP  ${TOOLCHAIN_DIR}/bin/arm-linux-gnueabihf-objdump)

# 查找策略
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
