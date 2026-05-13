#include "backend/rk/rk_inferer.h"

#include "core/logger.h"

namespace alg {

Status RkInferer::Load(const std::string& /*model_path*/) {
    ALG_LOGE("RkInferer is a stub; provide a real RKNN implementation");
    return ALG_E_BACKEND;
}

Status RkInferer::Forward() {
    return ALG_E_BACKEND;
}

}  // namespace alg
