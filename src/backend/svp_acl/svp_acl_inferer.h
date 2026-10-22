/**
 * @file svp_acl_inferer.h
 * @brief HiSilicon SVP ACL implementation of the chip-neutral IInferer.
 */

#ifndef ALG_BACKEND_SVP_ACL_SVP_ACL_INFERER_H
#define ALG_BACKEND_SVP_ACL_SVP_ACL_INFERER_H

#include <acl/svp_acl.h>
#include <acl/svp_acl_mdl.h>
#include <acl/svp_acl_rt.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#ifdef ALG_SVP_IVE_PREPROCESS
#include <ss_mpi_ive.h>
#endif

#include "core/infer/inferer.h"

namespace alg {

class SvpAclInferer : public IInferer {
  public:
    SvpAclInferer() = default;
    ~SvpAclInferer() override;

    Status Load(const std::string& model_path) override;
    Status Forward() override;
    bool SupportsDynamicAipp() const override;
    Status PrepareDynamicAipp(const AlgImage& image,
                              const PreprocessConfig& cfg,
                              PreprocessState& state,
                              SharedInputStaging* shared_staging) override;

  private:
    Status BuildViews();
    void FreeAll();

    uint32_t model_id_ = 0;
    bool model_loaded_ = false;
    bool runtime_acquired_ = false;
    bool initialized_ = false;
    void* model_memory_ = nullptr;
    size_t model_size_ = 0;
    svp_acl_mdl_desc* model_desc_ = nullptr;
    svp_acl_mdl_dataset* input_dataset_ = nullptr;
    svp_acl_mdl_dataset* output_dataset_ = nullptr;
    size_t image_input_index_ = static_cast<size_t>(-1);
    std::vector<size_t> output_raw_indices_;
    std::vector<void*> input_allocations_;
    std::vector<void*> output_allocations_;
#ifdef ALG_SVP_DYNAMIC_AIPP
    svp_acl_mdl_aipp* dynamic_aipp_ = nullptr;
    size_t dynamic_aipp_input_index_ = static_cast<size_t>(-1);
    size_t image_input_capacity_ = 0;
    void* own_image_input_ = nullptr;
    bool image_input_preflushed_ = false;
    bool split_copy_path_reported_ = false;
    bool cpu_prescale_reported_ = false;
#ifdef ALG_SVP_LIBYUV_FALLBACK
    /* DV500 CPU 预缩放的 1/2 折半中间帧（乒乓复用）。中间帧只被 libyuv/CPU
     * 读写，不绑定 ACL，因此用普通对齐内存即可。1080p 只需要一块 960x540。 */
    uint8_t* prescale_tmp_[2] = {nullptr, nullptr};
    size_t prescale_tmp_capacity_[2] = {0, 0};

    Status EnsurePrescaleTmp(int slot, size_t bytes);
    void FreePrescaleTmp();
#endif
#ifdef ALG_SVP_IVE_PREPROCESS
    td_phys_addr_t ive_output_phys_ = 0;
    void* ive_output_virt_ = nullptr;
    size_t ive_output_capacity_ = 0;
    td_phys_addr_t ive_assist_phys_ = 0;
    void* ive_assist_virt_ = nullptr;
    size_t ive_assist_capacity_ = 0;
    bool ive_path_reported_ = false;

    Status EnsureIveBuffers(int width, int height, size_t stride);
    void FreeIveBuffers();
#endif

    Status EnsureOwnImageInput();
#endif
};

}  // namespace alg

#endif  // ALG_BACKEND_SVP_ACL_SVP_ACL_INFERER_H
