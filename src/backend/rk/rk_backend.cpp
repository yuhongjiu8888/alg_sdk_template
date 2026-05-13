#include "backend/backend_factory.h"
#include "backend/rk/rk_inferer.h"

namespace alg {

std::unique_ptr<IInferer> MakeInferer() { return std::unique_ptr<IInferer>(new RkInferer()); }
const char*               BackendName() { return "rk"; }

}  // namespace alg
