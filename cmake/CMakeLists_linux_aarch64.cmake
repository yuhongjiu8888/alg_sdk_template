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
# OpenCV（SVP ACL 使用 v610 工具链对应的 Hisi 构建，其余后端沿用 SGK 构建）
# -----------------------------------------------------------------------------
if(ALG_BACKEND STREQUAL "svp_acl")
    set(SVP_OPENCV_ROOT
        "/root/opensource/opencv-4.5.0/build_arm_hisi/install"
        CACHE PATH "OpenCV install root for HiSilicon v610")
    include_directories(${SVP_OPENCV_ROOT}/include/opencv4)
    link_directories(
        ${SVP_OPENCV_ROOT}/lib
        ${SVP_OPENCV_ROOT}/lib/opencv4/3rdparty
    )
else()
    include_directories(/root/opensource/opencv-4.5.0/build_arm_sgk/install/include/opencv4)
    link_directories(
        /root/opensource/opencv-4.5.0/build_arm_sgk/install/lib
        /root/opensource/opencv-4.5.0/build_arm_sgk/install/lib/opencv4/3rdparty
    )
endif()
set(OPENCV4_LIB_STATIC
    libopencv_imgcodecs.a libopencv_imgproc.a libopencv_calib3d.a
    libopencv_features2d.a libopencv_flann.a libopencv_videoio.a
    libopencv_core.a liblibjpeg-turbo.a liblibopenjp2.a liblibpng.a
    libade.a libzlib.a libIlmImf.a liblibtiff.a
    libittnotify.a libquirc.a libzlib.a)

# -----------------------------------------------------------------------------
# jsoncpp（用户可通过 -DJSONCPP_ROOT=/path/to/jsoncpp/install 覆盖）
# 期望布局：${JSONCPP_ROOT}/include/json/json.h，${JSONCPP_ROOT}/lib/libjsoncpp.a
# -----------------------------------------------------------------------------
# if(NOT DEFINED JSONCPP_ROOT)
#     set(JSONCPP_ROOT "/root/opensource/jsoncpp/build_arm/install" CACHE PATH "jsoncpp install root")
# endif()
# message(STATUS "JSONCPP_ROOT = ${JSONCPP_ROOT}")
# include_directories(${JSONCPP_ROOT}/include)
# link_directories(${JSONCPP_ROOT}/lib)
# set(JSONCPP_LIB_STATIC libjsoncpp.a)

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
    src/core/log/alg_log.cpp
    src/core/registry/postprocessor_registry.cpp
    src/core/instance/model_instance.cpp
    src/core/preprocess/letterbox_preprocessor.cpp
    src/core/postprocess/nms.cpp
    src/core/solution/crop_util.cpp
    src/core/solution/chain_solution.cpp
)

# -----------------------------------------------------------------------------
# 模型（按类型注册的后处理）—— 加新模型在此处追加，无需改其它文件
#
# 当前分支聚焦：红绿灯检测 + 巴西限速牌识别（v3.4 OCR）+ 车牌识别（RTMDet + LPRNet）
#   - yolox_det           : mmyolo YOLOXHead 多尺度（红绿灯 4 类，stride 8/16/32）
#   - yolov5_anchor_det   : 单尺度 anchor 解码（SpeedSignNet 1 类 stride=8 anchor=(36,36)）
#   - ocr_classifier      : 三头逐位数字 OCR + class_names 驱动解码 + 置信度过滤（限速牌 12 类）
#   - dualhead_classifier : 旧双头数字识别（限速牌 9 类）；保留向后兼容，新配置走 ocr_classifier
#   - rtmdet_det          : RTMDet 多尺度单类检测（车牌，cls/bbox × stride 8/16/32）
#   - lprnet_rec          : LPRNet CTC 车牌识别（32 时间步 × 37 类字符）
# -----------------------------------------------------------------------------
set(ALG_MODEL_SRCS
    src/models/yolox_det/yolox_det_postprocessor.cpp
    src/models/yolox_det/yolox_det_register.cpp
    src/models/yolov5_anchor_det/yolov5_anchor_det_postprocessor.cpp
    src/models/yolov5_anchor_det/yolov5_anchor_det_register.cpp
    src/models/ocr_classifier/ocr_classifier_postprocessor.cpp
    src/models/ocr_classifier/ocr_classifier_register.cpp
    src/models/dualhead_classifier/dualhead_classifier_postprocessor.cpp
    src/models/dualhead_classifier/dualhead_classifier_register.cpp
    src/models/rtmdet_det/rtmdet_det_postprocessor.cpp
    src/models/rtmdet_det/rtmdet_det_register.cpp
    src/models/lprnet_rec/lprnet_rec_postprocessor.cpp
    src/models/lprnet_rec/lprnet_rec_register.cpp
)

#第三方库json
set(JSONCPP_SRCS src/json/jsoncpp.cpp)

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
                        /root/sgk-sdk/include /root/project/sgk/deploy/sgk-sdk/include)
    link_directories(${CMAKE_SOURCE_DIR}/lib/static
                     /root/sgk-sdk/lib/static /root/project/sgk/deploy/sgk-sdk/lib/static)
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
elseif(ALG_BACKEND STREQUAL "svp_acl")
    set(ALG_BACKEND_SRCS
        src/backend/svp_acl/svp_acl_inferer.cpp
        src/backend/svp_acl/svp_acl_backend.cpp
    )
    set(SVP_ACL_ROOT
        "/root/project/haisi-v610/acllib"
        CACHE PATH "HiSilicon SVP ACLlib root")
    set(SVP_ACL_LIB_DIR
        "${SVP_ACL_ROOT}/lib32_arm-v01c02-linux-musleabi/stub"
        CACHE PATH "SVP ACL library directory")
    set(HISI_SECUREC_LIB_DIR
        "/root/project/haisi-v610/gcc-20250305-arm-v01c02-linux-musleabi/arm-v01c02-linux-musleabi-gcc/target/usr/lib/a7_softfp_neon-vfpv4"
        CACHE PATH "HiSilicon securec library directory")
    include_directories(${SVP_ACL_ROOT}/include)
    set(LIBYUV_ROOT
        "${CMAKE_SOURCE_DIR}/third_party/libyuv/hi3516"
        CACHE PATH "libyuv root for split-plane NV12/NV21 preprocessing")
    if(ALG_SVP_DYNAMIC_AIPP)
        if(NOT EXISTS "${LIBYUV_ROOT}/include/libyuv/scale.h" OR
           NOT EXISTS "${LIBYUV_ROOT}/lib/libyuv.a")
            message(FATAL_ERROR "libyuv headers/library not found under ${LIBYUV_ROOT}")
        endif()
        include_directories(${LIBYUV_ROOT}/include)
    endif()
    link_directories(${SVP_ACL_LIB_DIR} ${HISI_SECUREC_LIB_DIR})
    set(ALG_BACKEND_LIBS
        libprotobuf-c.a
        libss_mpi_sysmem.a
        libsvp_acl.a
        libsecurec.a
    )
    if(ALG_SVP_DYNAMIC_AIPP)
        list(APPEND ALG_BACKEND_LIBS "${LIBYUV_ROOT}/lib/libyuv.a")
    endif()
else()
    message(FATAL_ERROR
        "Unknown ALG_BACKEND: ${ALG_BACKEND} (expected xmm, svp_acl or rk)")
endif()

# -----------------------------------------------------------------------------
# 共享库
# -----------------------------------------------------------------------------
add_library(alg_sdk SHARED
    ${ALG_CORE_SRCS}
    ${ALG_MODEL_SRCS}
    ${ALG_BACKEND_SRCS}
    ${JSONCPP_SRCS}
)
target_compile_definitions(alg_sdk PRIVATE ALG_BUILDING_SDK=1)
if(ALG_BACKEND STREQUAL "svp_acl")
    # SVP ACL/OpenCV 均为静态库，循环依赖需要 linker group。
    target_link_libraries(alg_sdk
        -Wl,--start-group
        ${OPENCV4_LIB_STATIC}
        ${ALG_BACKEND_LIBS}
        -Wl,--end-group
        rt dl pthread
    )
else()
    target_link_libraries(alg_sdk
        ${OPENCV4_LIB_STATIC}
        # ${JSONCPP_LIB_STATIC}
        ${ALG_BACKEND_LIBS}
        rt dl pthread
    )
endif()
# Android logcat sink 需要 liblog（NDK 构建时）
if(ANDROID OR ALG_LOG_ANDROID)
    target_link_libraries(alg_sdk log)
endif()

# -----------------------------------------------------------------------------
# 测试可执行（通用 runner，跑哪个 solution 由 JSON 决定）
# -----------------------------------------------------------------------------
add_executable(test_runner test/test_runner.cpp)
target_link_libraries(test_runner alg_sdk pthread)

# 循环压测 runner：同一帧（图片 / NV12 yuv.bin）连续推理，测稳定性与 FPS。
add_executable(loop_runner test/loop_runner.cpp)
target_link_libraries(loop_runner alg_sdk pthread)
