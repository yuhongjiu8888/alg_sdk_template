#include "core/registry/postprocessor_registry.h"
#include "models/yolov5_anchor_det/yolov5_anchor_det_postprocessor.h"

namespace alg {

REGISTER_ALG_POST("yolov5_anchor_det", []{
    return std::unique_ptr<IPostprocessor>(new Yolov5AnchorDetPostprocessor());
});

}  // namespace alg
