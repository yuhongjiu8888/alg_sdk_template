#include "core/registry/postprocessor_registry.h"
#include "models/face_attribute/face_attribute_postprocessor.h"

namespace alg {

REGISTER_ALG_POST("face_attribute", []{
    return std::unique_ptr<IPostprocessor>(new FaceAttributePostprocessor());
});

}  // namespace alg
