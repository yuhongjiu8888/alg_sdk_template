#include "core/registry/postprocessor_registry.h"
#include "models/dualhead_classifier/dualhead_classifier_postprocessor.h"

namespace alg {

REGISTER_ALG_POST("dualhead_classifier", []{
    return std::unique_ptr<IPostprocessor>(new DualheadClassifierPostprocessor());
});

}  // namespace alg
