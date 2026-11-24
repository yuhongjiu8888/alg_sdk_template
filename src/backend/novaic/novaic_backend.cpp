#include "backend/backend_factory.h"
#include "backend/novaic/novaic_inferer.h"

namespace alg {

std::unique_ptr<IInferer> MakeInferer() {
    return std::unique_ptr<IInferer>(new NovaicInferer());
}

const char* BackendName() {
    return "novaic";
}

}  // namespace alg
