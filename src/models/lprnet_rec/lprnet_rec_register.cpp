#include "core/registry/postprocessor_registry.h"
#include "models/lprnet_rec/lprnet_rec_postprocessor.h"

namespace alg {

REGISTER_ALG_POST("lprnet_rec", []{
    return std::unique_ptr<IPostprocessor>(new LprnetRecPostprocessor());
});

}  // namespace alg
