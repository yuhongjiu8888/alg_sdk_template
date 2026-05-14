#include "backend/backend_factory.h"
#include "backend/mnn/mnn_inferer.h"

namespace alg {

std::unique_ptr<IInferer> MakeInferer() { return std::unique_ptr<IInferer>(new MnnInferer()); }
const char*               BackendName() { return "mnn"; }

}  // namespace alg
