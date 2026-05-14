cmake_minimum_required(VERSION 3.10)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_BUILD_TYPE MATCHES "Release")
    set(CMAKE_CXX_FLAGS "-O3 -s -Wl,--gc-sections -ffunction-sections -fdata-sections -ffast-math ${CMAKE_CXX_FLAGS}")
    set(CMAKE_C_FLAGS   "-O3 -s -Wl,--gc-sections -ffunction-sections -fdata-sections -ffast-math ${CMAKE_C_FLAGS}")
elseif(CMAKE_BUILD_TYPE MATCHES "Debug")
    set(CMAKE_CXX_FLAGS "-g3 -O0 ${CMAKE_CXX_FLAGS}")
    set(CMAKE_C_FLAGS   "-g3 -O0 ${CMAKE_C_FLAGS}")
else()
    message(FATAL_ERROR "CMAKE_BUILD_TYPE must be Debug or Release")
endif()

set(CMAKE_CXX_FLAGS "-std=c++14 -Wall -fpermissive -fPIC -fvisibility=hidden ${CMAKE_CXX_FLAGS}")
set(CMAKE_C_FLAGS   "-Wall -fpermissive -fPIC -fvisibility=hidden ${CMAKE_C_FLAGS}")

# -----------------------------------------------------------------------------
# OpenCV (x86_64 host)
# 用户可通过 -DOPENCV_ROOT=/path/to/opencv 覆盖
# 期望布局: ${OPENCV_ROOT}/include/opencv4, ${OPENCV_ROOT}/lib
# -----------------------------------------------------------------------------
if(NOT DEFINED OPENCV_ROOT)
    set(OPENCV_ROOT "/usr" CACHE PATH "OpenCV install root")
endif()
message(STATUS "OPENCV_ROOT = ${OPENCV_ROOT}")

include_directories(${OPENCV_ROOT}/local/include/opencv4)
link_directories(${OPENCV_ROOT}/lib)
set(OPENCV4_LIBS opencv_imgcodecs opencv_imgproc opencv_core)

# -----------------------------------------------------------------------------
# jsoncpp
# -----------------------------------------------------------------------------
# if(NOT DEFINED JSONCPP_ROOT)
#     set(JSONCPP_ROOT "/usr" CACHE PATH "jsoncpp install root")
# endif()
# message(STATUS "JSONCPP_ROOT = ${JSONCPP_ROOT}")
# include_directories(${JSONCPP_ROOT}/include)
# link_directories(${JSONCPP_ROOT}/lib)
# set(JSONCPP_LIB_STATIC jsoncpp)

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
# 模型
# -----------------------------------------------------------------------------
set(ALG_MODEL_SRCS
    src/models/yolox_det/yolox_det_postprocessor.cpp
    src/models/yolox_det/yolox_det_register.cpp
    src/models/yolov5_anchor_det/yolov5_anchor_det_postprocessor.cpp
    src/models/yolov5_anchor_det/yolov5_anchor_det_register.cpp
    src/models/dualhead_classifier/dualhead_classifier_postprocessor.cpp
    src/models/dualhead_classifier/dualhead_classifier_register.cpp
)

#第三方库json
set(JSONCPP_SRCS src/json/jsoncpp.cpp)

# -----------------------------------------------------------------------------
# 后端（x86_64 本地验证只支持 mnn）
# -----------------------------------------------------------------------------
set(ALG_BACKEND_SRCS)
set(ALG_BACKEND_LIBS)
if(ALG_BACKEND STREQUAL "mnn")
    set(ALG_BACKEND_SRCS
        src/backend/mnn/mnn_inferer.cpp
        src/backend/mnn/mnn_backend.cpp
    )
    if(NOT DEFINED MNN_ROOT)
        set(MNN_ROOT "/root/opensource/mnn" CACHE PATH "MNN install root")
    endif()
    message(STATUS "MNN_ROOT = ${MNN_ROOT}")
    include_directories(${MNN_ROOT}/V2.5.1/include)
    link_directories(${MNN_ROOT}/V2.5.1/lib/linux_x86_64)
    set(ALG_BACKEND_LIBS MNN)
else()
    message(FATAL_ERROR "x86_64 platform only supports ALG_BACKEND=mnn, got: ${ALG_BACKEND}")
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
target_link_libraries(alg_sdk
    ${OPENCV4_LIBS}
    ${ALG_BACKEND_LIBS}
    rt dl pthread
)

# -----------------------------------------------------------------------------
# 测试可执行（通用 runner，跑哪个 solution 由 JSON 决定）
# -----------------------------------------------------------------------------
add_executable(test_runner test/test_runner.cpp)
target_link_libraries(test_runner alg_sdk pthread)
