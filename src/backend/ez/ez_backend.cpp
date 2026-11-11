#include "backend/backend_factory.h"
#include "backend/ez/ez_inferer.h"

namespace alg {

std::unique_ptr<IInferer> MakeInferer() {
    return std::unique_ptr<IInferer>(new EzInferer());
}

const char* BackendName() {
    return "ez";
}

}  // namespace alg
