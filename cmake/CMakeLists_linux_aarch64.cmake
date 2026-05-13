cmake_minimum_required(VERSION 3.10)

if(DEFINED CMAKE_TOOLCHAIN_FILE)
    set(LIBRARY_OUTPUT_PATH_ROOT ${CMAKE_BINARY_DIR}
        CACHE PATH "library output root")
    get_filename_component(_tc_name ${CMAKE_TOOLCHAIN_FILE} NAME)
    find_file(CMAKE_TOOLCHAIN_FILE ${_tc_name} PATHS ${CMAKE_SOURCE_DIR} NO_DEFAULT_PATH)
    message(STATUS "CMAKE_TOOLCHAIN_FILE = ${CMAKE_TOOLCHAIN_FILE}")
endif()

if(CMAKE_BUILD_TYPE MATCHES "Release")
    set(CMAKE_CXX_FLAGS "-s ${CMAKE_CXX_FLAGS}")
    set(CMAKE_C_FLAGS   "-s ${CMAKE_C_FLAGS}")
elseif(CMAKE_BUILD_TYPE MATCHES "Debug")
    set(CMAKE_CXX_FLAGS "-g3 ${CMAKE_CXX_FLAGS}")
    set(CMAKE_C_FLAGS   "-g3 ${CMAKE_C_FLAGS}")
else()
    message(FATAL_ERROR "CMAKE_BUILD_TYPE must be Debug or Release")
endif()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS "-std=c++14 -O3 -s -Wl,--gc-sections -Wall -fpermissive -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections -ffast-math ${CMAKE_CXX_FLAGS}")
set(CMAKE_C_FLAGS   "-fpermissive -O3 -s -Wl,--gc-sections -Wall -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections -ffast-math ${CMAKE_C_FLAGS}")

# -----------------------------------------------------------------------------
# OpenCV
# -----------------------------------------------------------------------------
include_directories(/root/opensource/opencv-4.5.0/build_arm_sgk/install/include/opencv4)
link_directories(
    /root/opensource/opencv-4.5.0/build_arm_sgk/install/lib
    /root/opensource/opencv-4.5.0/build_arm_sgk/install/lib/opencv4/3rdparty
)
set(OPENCV4_LIB_STATIC
    libopencv_imgcodecs.a libopencv_imgproc.a libopencv_calib3d.a
    libopencv_features2d.a libopencv_flann.a libopencv_videoio.a
    libopencv_core.a liblibjpeg-turbo.a liblibopenjp2.a liblibpng.a
    liblibwebp.a libade.a libzlib.a libIlmImf.a liblibtiff.a
    libittnotify.a libquirc.a libzlib.a)

# -----------------------------------------------------------------------------
# jsoncpp（用户可通过 -DJSONCPP_ROOT=/path/to/jsoncpp/install 覆盖）
# 期望布局：${JSONCPP_ROOT}/include/json/json.h，${JSONCPP_ROOT}/lib/libjsoncpp.a
# -----------------------------------------------------------------------------
if(NOT DEFINED JSONCPP_ROOT)
    set(JSONCPP_ROOT "/root/opensource/jsoncpp/build_arm/install" CACHE PATH "jsoncpp install root")
endif()
message(STATUS "JSONCPP_ROOT = ${JSONCPP_ROOT}")
include_directories(${JSONCPP_ROOT}/include)
link_directories(${JSONCPP_ROOT}/lib)
set(JSONCPP_LIB_STATIC libjsoncpp.a)

# -----------------------------------------------------------------------------
# Public + internal include paths
# -----------------------------------------------------------------------------
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

# -----------------------------------------------------------------------------
# 核心（与芯片、模型都无关）
# -----------------------------------------------------------------------------
set(ALG_CORE_SRCS
    src/interface/alg_interface.cpp
    src/core/object.cpp
    src/core/config/config.cpp
    src/core/registry/postprocessor_registry.cpp
    src/core/instance/model_instance.cpp
    src/core/preprocess/letterbox_preprocessor.cpp
    src/core/postprocess/nms.cpp
    src/core/solution/crop_util.cpp
    src/core/solution/chain_solution.cpp
)

# -----------------------------------------------------------------------------
# 模型（按类型注册的后处理）—— 加新模型在此处追加，无需改其它文件
# -----------------------------------------------------------------------------
set(ALG_MODEL_SRCS
    src/models/fcos_face/fcos_face_postprocessor.cpp
    src/models/fcos_face/fcos_face_register.cpp
    src/models/pfld_landmark/pfld_landmark_postprocessor.cpp
    src/models/pfld_landmark/pfld_landmark_register.cpp
    src/models/face_attribute/face_attribute_postprocessor.cpp
    src/models/face_attribute/face_attribute_register.cpp
)

# -----------------------------------------------------------------------------
# 后端（一份产物对应一种芯片）
# -----------------------------------------------------------------------------
set(ALG_BACKEND_SRCS)
set(ALG_BACKEND_LIBS)
if(ALG_BACKEND STREQUAL "xmm")
    set(ALG_BACKEND_SRCS
        src/backend/xmm/xmm_inferer.cpp
        src/backend/xmm/xmm_backend.cpp
    )
    include_directories(${CMAKE_SOURCE_DIR}/include/xmm_sdk
                        /root/sgk-sdk/include)
    link_directories(${CMAKE_SOURCE_DIR}/lib/static
                     /root/sgk-sdk/lib/static)
    set(ALG_BACKEND_LIBS
        libxmedia_cl.a
        libxmedia_npu.a
        libxmedia_common.a
        libxmedia_flatcc.a
    )
elseif(ALG_BACKEND STREQUAL "rk")
    set(ALG_BACKEND_SRCS
        src/backend/rk/rk_inferer.cpp
        src/backend/rk/rk_backend.cpp
    )
else()
    message(FATAL_ERROR "Unknown ALG_BACKEND: ${ALG_BACKEND} (expected xmm or rk)")
endif()

# -----------------------------------------------------------------------------
# 共享库
# -----------------------------------------------------------------------------
add_library(alg_sdk SHARED
    ${ALG_CORE_SRCS}
    ${ALG_MODEL_SRCS}
    ${ALG_BACKEND_SRCS}
)
target_compile_definitions(alg_sdk PRIVATE ALG_BUILDING_SDK=1)
target_link_libraries(alg_sdk
    ${OPENCV4_LIB_STATIC}
    ${JSONCPP_LIB_STATIC}
    ${ALG_BACKEND_LIBS}
    rt dl pthread
)

# -----------------------------------------------------------------------------
# 测试可执行
# -----------------------------------------------------------------------------
add_executable(test_facedet test/test_facedet.cpp)
target_link_libraries(test_facedet alg_sdk pthread)
