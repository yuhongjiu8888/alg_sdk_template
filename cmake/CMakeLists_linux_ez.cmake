# EZ826 / JC371 Linux backend. No OpenCV headers or libraries are required.
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "EZ826/JC371 requires a 32-bit ARM toolchain; use toolchain/ez_linux.toolchain.cmake")
endif()
if(NOT CMAKE_BUILD_TYPE MATCHES "^(Debug|Release|RelWithDebInfo|MinSizeRel)$")
    message(FATAL_ERROR "CMAKE_BUILD_TYPE must be Debug, Release, RelWithDebInfo or MinSizeRel")
endif()

# Configure the EZ SDK installation here before running build.sh/builddeploy.sh.
# The scripts accept the same target arguments as the HiSilicon build; no SDK
# path arguments or environment variables are needed.
set(EZ_NDK_ROOT "/home/yuwei/alg_sdk/chip_sdk/eezaytech/ezndk20-2026-07-30/ndks/ndk-826")
set(EZ_NDK_LIB_DIR "${EZ_NDK_ROOT}")
set(EZ_PLATFORM_LIB_DIR "${EZ_NDK_LIB_DIR}")
set(EZ_PLATFORM_BOARD_LIB_DIR "${EZ_NDK_LIB_DIR}/board_lib")
# Same platform dependencies as examples/826_JC371/*/Virgo.mk (TARGET_FRAMEWORK=MDP).
set(EZ_PLATFORM_LIBS log mbase ax)
set(ALG_BUILD_EZ_RUNNER ON)

if(NOT EXISTS "${EZ_NDK_ROOT}/include/libnn/net_api.h" OR
   NOT EXISTS "${EZ_NDK_ROOT}/include/VX_EXT/vx_ext_amd.h")
    message(FATAL_ERROR
        "EZ NDK headers not found under ${EZ_NDK_ROOT}. Extract ndks/826(823)-ndk.zip "
        "and configure EZ_NDK_ROOT in cmake/CMakeLists_linux_ez.cmake.")
endif()

set(ALG_EZ_LIBRARIES)
set(ALG_EZ_RUNTIME_LIBRARIES)
foreach(_ez_name openvx openvx-nn nn)
    # Use the supplied target shared libraries explicitly, never a host OpenVX.
    set(_ez_path "${EZ_NDK_LIB_DIR}/lib${_ez_name}.so")
    if(NOT EXISTS "${_ez_path}")
        message(FATAL_ERROR "Missing EZ target library ${_ez_path}; set EZ_NDK_LIB_DIR")
    endif()
    list(APPEND ALG_EZ_LIBRARIES "${_ez_path}")
    list(APPEND ALG_EZ_RUNTIME_LIBRARIES "${_ez_path}")
endforeach()
# set(_ez_path "${EZ_NDK_LIB_DIR}/board_lib/lib${_ez_name}.so")
foreach(_ez_name IN LISTS EZ_PLATFORM_LIBS)
    unset(_ez_platform_library CACHE)
    unset(_ez_platform_library)
    find_library(_ez_platform_library NAMES "${_ez_name}"
        PATHS "${EZ_PLATFORM_BOARD_LIB_DIR}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    if(NOT _ez_platform_library)
        message(FATAL_ERROR
            "Missing EZ platform library '${_ez_name}' under ${EZ_PLATFORM_BOARD_LIB_DIR}; "
            "set EZ_PLATFORM_BOARD_LIB_DIR to the board SDK library directory")
    endif()
    list(APPEND ALG_EZ_LIBRARIES "${_ez_platform_library}")
endforeach()
unset(_ez_platform_library CACHE)

# libalg_sdk.so records the NDK libraries by SONAME, so every consumer (e.g. the
# runner) must let ld resolve those transitive dependencies. "-L" is not enough
# for needed libraries on this binutils, and default system paths must not be
# used; point ld explicitly at both SDK directories.
set(ALG_EZ_RPATH_LINK_FLAGS
    "-Wl,-rpath-link,${EZ_NDK_LIB_DIR} -Wl,-rpath-link,${EZ_PLATFORM_BOARD_LIB_DIR}")

# Tell the packaging script exactly which configured NDK libraries to copy.
# Keep this independent of CMakeCache: paths are edited directly above.
string(REPLACE ";" "\n" _ez_runtime_manifest "${ALG_EZ_RUNTIME_LIBRARIES}")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/ez_runtime_libraries.txt"
    CONTENT "${_ez_runtime_manifest}\n")

set(ALG_CORE_SRCS
    src/interface/alg_interface.cpp
    src/core/object.cpp
    src/core/config/config.cpp
    src/core/log/alg_log.cpp
    src/core/registry/postprocessor_registry.cpp
    src/core/instance/model_instance.cpp
    src/core/postprocess/nms.cpp
    src/core/solution/crop_geometry.cpp
    src/core/solution/chain_solution.cpp
)
set(ALG_MODEL_SRCS
    src/models/yolox_det/yolox_det_postprocessor.cpp
    src/models/yolox_det/yolox_det_register.cpp
    src/models/yolov5_anchor_det/yolov5_anchor_det_postprocessor.cpp
    src/models/yolov5_anchor_det/yolov5_anchor_det_register.cpp
    src/models/ocr_classifier/ocr_classifier_postprocessor.cpp
    src/models/ocr_classifier/ocr_classifier_register.cpp
    src/models/rtmdet_det/rtmdet_det_postprocessor.cpp
    src/models/rtmdet_det/rtmdet_det_register.cpp
    src/models/lprnet_rec/lprnet_rec_postprocessor.cpp
    src/models/lprnet_rec/lprnet_rec_register.cpp
)
add_library(alg_sdk SHARED
    ${ALG_CORE_SRCS}
    ${ALG_MODEL_SRCS}
    src/backend/ez/ez_inferer.cpp
    src/backend/ez/ez_backend.cpp
    src/json/jsoncpp.cpp
)
target_include_directories(alg_sdk PUBLIC "${CMAKE_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_SOURCE_DIR}/src"
            "${EZ_NDK_ROOT}/include"
            "${EZ_NDK_ROOT}/include/VX_EXT"
            "${EZ_NDK_ROOT}/include/libnn")
target_compile_definitions(alg_sdk PRIVATE ALG_BUILDING_SDK=1 PUBLIC ALG_NO_OPENCV=1)
target_compile_options(alg_sdk PRIVATE
    -Wall -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections)
# Preserve IEEE checks used for geometry and quantization; do not enable -ffast-math.
set_property(TARGET alg_sdk APPEND_STRING PROPERTY LINK_FLAGS
    " -Wl,--gc-sections ${ALG_EZ_RPATH_LINK_FLAGS}")
target_link_libraries(alg_sdk PRIVATE ${ALG_EZ_LIBRARIES} rt dl pthread m)
# Deployments keep NDK runtime libraries next to the application under lib/.
set_target_properties(alg_sdk PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "$ORIGIN/lib;$ORIGIN")
if(ANDROID OR ALG_LOG_ANDROID)
    target_link_libraries(alg_sdk PRIVATE log)
endif()
if(ALG_BUILD_EZ_RUNNER)
    add_executable(ez_nv21_runner test/ez_nv21_runner.cpp)
    target_link_libraries(ez_nv21_runner PRIVATE alg_sdk pthread)
    set_target_properties(ez_nv21_runner PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "$ORIGIN;$ORIGIN/lib")
    set_property(TARGET ez_nv21_runner APPEND_STRING PROPERTY LINK_FLAGS
        " ${ALG_EZ_RPATH_LINK_FLAGS}")
endif()
message(STATUS "EZ_NDK_ROOT = ${EZ_NDK_ROOT}")
message(STATUS "EZ_PLATFORM_LIB_DIR = ${EZ_PLATFORM_LIB_DIR}")
