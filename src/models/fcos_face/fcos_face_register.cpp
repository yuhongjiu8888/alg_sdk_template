#include "core/registry/postprocessor_registry.h"
#include "models/fcos_face/fcos_face_postprocessor.h"

namespace alg {

REGISTER_ALG_POST("fcos_face", []{
    return std::unique_ptr<IPostprocessor>(new FcosFacePostprocessor());
});

}  // namespace alg
