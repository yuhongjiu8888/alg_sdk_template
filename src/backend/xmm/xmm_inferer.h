/**
 * @file xmm_inferer.h
 * @brief IInferer implementation for the XMM chip (xmedia_cl + MMZ).
 *
 * This is the only file you reimplement to port the SDK to another chip.
 * All preprocess/postprocess code is unchanged across backends.
 */

#ifndef ALG_BACKEND_XMM_XMM_INFERER_H
#define ALG_BACKEND_XMM_XMM_INFERER_H

#include "core/infer/inferer.h"

#include "xmedia_cl.h"
#include "xmedia_cl_common.h"
#include "xmedia_mmz.h"
#include "xmedia_sys.h"

namespace alg {

class XmmInferer : public IInferer {
  public:
    XmmInferer();
    ~XmmInferer() override;

    Status Load(const std::string& model_path) override;
    Status Forward() override;

  private:
    Status AllocateTensorMem(xmedia_cl_tensor_info_inout* inout);
    void   FreeAll();
    Status BuildViews();

    xmedia_cl_context     context_ = nullptr;
    xmedia_cl_graph       graph_ = nullptr;
    xmedia_cl_device_id*  devices_ = nullptr;
    xmedia_cl_u32         num_devices_ = 0;

    xmedia_u64 phy_workspace_ = 0, phy_weight_ = 0, phy_input_ = 0, phy_output_ = 0;
    void*      vir_workspace_ = nullptr;
    void*      vir_weight_ = nullptr;
    void*      vir_input_ = nullptr;
    void*      vir_output_ = nullptr;
    xmedia_u32 input_total_size_ = 0;
    xmedia_u32 output_total_size_ = 0;

    xmedia_cl_tensor_info_inout cl_input_{};
    xmedia_cl_tensor_info_inout cl_output_{};

    bool initialized_ = false;
};

}  // namespace alg

#endif  // ALG_BACKEND_XMM_XMM_INFERER_H
