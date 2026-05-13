#include "backend/backend_factory.h"
#include "backend/xmm/xmm_inferer.h"

namespace alg {

std::unique_ptr<IInferer> MakeInferer() { return std::unique_ptr<IInferer>(new XmmInferer()); }
const char*               BackendName() { return "xmm"; }

}  // namespace alg
