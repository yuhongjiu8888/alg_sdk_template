set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(TOOLCHAIN_DIR /root/project/sgk/XMIPCLinuxV100R005C00SPC030/tools/linux/toolchains/arm-gcc12.2.0-linux-uclibceabi/)

# 编译器
set(CMAKE_C_COMPILER   ${TOOLCHAIN_DIR}/bin/arm-gcc12.2.0-linux-uclibceabi-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_DIR}/bin/arm-gcc12.2.0-linux-uclibceabi-g++)

# sysroot
set(CMAKE_SYSROOT ${TOOLCHAIN_DIR}/sysroot)
set(CMAKE_FIND_ROOT_PATH ${CMAKE_SYSROOT})

# binutils
set(CMAKE_AR       ${TOOLCHAIN_DIR}/bin/arm-gcc12.2.0-linux-uclibceabi-ar)
set(CMAKE_RANLIB   ${TOOLCHAIN_DIR}/bin/arm-gcc12.2.0-linux-uclibceabi-ranlib)
set(CMAKE_STRIP    ${TOOLCHAIN_DIR}/bin/arm-gcc12.2.0-linux-uclibceabi-strip)
set(CMAKE_OBJCOPY  ${TOOLCHAIN_DIR}/bin/arm-gcc12.2.0-linux-uclibceabi-objcopy)
set(CMAKE_OBJDUMP  ${TOOLCHAIN_DIR}/bin/arm-gcc12.2.0-linux-uclibceabi-objdump)

# 查找策略
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
