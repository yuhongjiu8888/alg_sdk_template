set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(NOVAIC_TOOLCHAIN_DIR "/home/yuwei/work_dir/test/novaic/arm-ca53-linux-gnueabihf-10.4.0"
    CACHE PATH "NT98332 ARM hard-float toolchain")
set(_prefix "${NOVAIC_TOOLCHAIN_DIR}/bin/arm-ca53-linux-gnueabihf-")
set(CMAKE_C_COMPILER   "${_prefix}gcc")
set(CMAKE_CXX_COMPILER "${_prefix}g++")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
