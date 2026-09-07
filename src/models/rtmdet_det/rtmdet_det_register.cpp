#include "core/registry/postprocessor_registry.h"
#include "models/rtmdet_det/rtmdet_det_postprocessor.h"

namespace alg {

REGISTER_ALG_POST("rtmdet_det", []{
    return std::unique_ptr<IPostprocessor>(new RtmdetDetPostprocessor());
});

}  // namespace alg
