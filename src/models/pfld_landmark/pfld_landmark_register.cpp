#include "core/registry/postprocessor_registry.h"
#include "models/pfld_landmark/pfld_landmark_postprocessor.h"

namespace alg {

REGISTER_ALG_POST("pfld_landmark", []{
    return std::unique_ptr<IPostprocessor>(new PfldLandmarkPostprocessor());
});

}  // namespace alg
