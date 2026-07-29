/**
 * @file svp_acl_inferer.h
 * @brief HiSilicon SVP ACL implementation of the chip-neutral IInferer.
 */

#ifndef ALG_BACKEND_SVP_ACL_SVP_ACL_INFERER_H
#define ALG_BACKEND_SVP_ACL_SVP_ACL_INFERER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <acl/svp_acl.h>
#include <acl/svp_acl_mdl.h>
#include <acl/svp_acl_rt.h>

#include "core/infer/inferer.h"

namespace alg {

class SvpAclInferer : public IInferer {
  public:
    SvpAclInferer() = default;
    ~SvpAclInferer() override;

    Status Load(const std::string& model_path) override;
    Status Forward() override;

  private:
    Status BuildViews();
    void   FreeAll();

    uint32_t             model_id_ = 0;
    bool                 model_loaded_ = false;
    bool                 runtime_acquired_ = false;
    bool                 initialized_ = false;
    void*                model_memory_ = nullptr;
    size_t               model_size_ = 0;
    svp_acl_mdl_desc*     model_desc_ = nullptr;
    svp_acl_mdl_dataset*  input_dataset_ = nullptr;
    svp_acl_mdl_dataset*  output_dataset_ = nullptr;
    size_t                image_input_index_ = static_cast<size_t>(-1);
    std::vector<size_t>   output_raw_indices_;
};

}  // namespace alg

#endif  // ALG_BACKEND_SVP_ACL_SVP_ACL_INFERER_H
