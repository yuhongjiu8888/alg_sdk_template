#include "core/registry/postprocessor_registry.h"
#include "models/yolox_det/yolox_det_postprocessor.h"

namespace alg {

REGISTER_ALG_POST("yolox_det", []{
    return std::unique_ptr<IPostprocessor>(new YoloxDetPostprocessor());
});

}  // namespace alg
