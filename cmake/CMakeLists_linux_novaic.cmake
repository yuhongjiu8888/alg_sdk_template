set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4 OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^arm")
    message(FATAL_ERROR "NT98332 requires the 32-bit ARM hard-float toolchain")
endif()

set(_nt98332 "${CMAKE_SOURCE_DIR}/third_party/NT98332")
add_library(alg_sdk SHARED
    src/backend/novaic/novaic_backend.cpp
    src/backend/novaic/novaic_inferer.cpp
    src/core/config/config.cpp
    src/core/instance/model_instance.cpp
    src/core/log/alg_log.cpp
    src/core/object.cpp
    src/core/postprocess/nms.cpp
    src/core/registry/postprocessor_registry.cpp
    src/core/solution/chain_solution.cpp
    src/core/solution/crop_geometry.cpp
    src/interface/alg_interface.cpp
    src/json/jsoncpp.cpp
    src/models/lprnet_rec/lprnet_rec_postprocessor.cpp
    src/models/lprnet_rec/lprnet_rec_register.cpp
    src/models/ocr_classifier/ocr_classifier_postprocessor.cpp
    src/models/ocr_classifier/ocr_classifier_register.cpp
    src/models/rtmdet_det/rtmdet_det_postprocessor.cpp
    src/models/rtmdet_det/rtmdet_det_register.cpp
    src/models/yolov5_anchor_det/yolov5_anchor_det_postprocessor.cpp
    src/models/yolov5_anchor_det/yolov5_anchor_det_register.cpp
)
target_include_directories(alg_sdk PUBLIC include PRIVATE src)
target_include_directories(alg_sdk SYSTEM PRIVATE "${_nt98332}/include")
target_compile_definitions(alg_sdk PRIVATE ALG_BUILDING_SDK=1 ALG_NO_OPENCV=1 __LINUX __LINUX_USER__)
target_compile_options(alg_sdk PRIVATE -Wall -fvisibility=hidden)
# Some HDAL libraries have no SONAME. Link by basename so the board loader can relocate them.
target_link_libraries(alg_sdk PRIVATE "-L${_nt98332}/lib"
    vendor_ai2_pub vendor_ai2 prebuilt_ai vendor_media hdal vos pthread dl m)
set_target_properties(alg_sdk PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "$ORIGIN/lib"
    LINK_FLAGS "-Wl,--no-undefined,-rpath-link,${_nt98332}/lib")

add_executable(test_runner test/ez_nv21_runner.cpp)
target_link_libraries(test_runner PRIVATE alg_sdk)
set_target_properties(test_runner PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "$ORIGIN;$ORIGIN/lib"
    LINK_FLAGS "-Wl,-rpath-link,${_nt98332}/lib")
