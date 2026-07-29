#include "backend/backend_factory.h"
#include "backend/svp_acl/svp_acl_inferer.h"

namespace alg {

std::unique_ptr<IInferer> MakeInferer() {
    return std::unique_ptr<IInferer>(new SvpAclInferer());
}

const char* BackendName() { return "svp_acl"; }

}  // namespace alg
