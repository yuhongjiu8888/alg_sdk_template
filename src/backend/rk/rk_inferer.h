/**
 * @file rk_inferer.h
 * @brief IInferer stub for Rockchip RKNN — illustrates the chip-port surface.
 *
 * To activate: replace the stub with rknn_api.h calls (rknn_init,
 * rknn_query, rknn_inputs_set, rknn_run, rknn_outputs_get). The
 * TensorView shape it exposes uses the SAME core abstractions, so the
 * generic LetterboxPreprocessor and every IPostprocessor (FCOS,
 * YOLOv5, ...) keep working unchanged.
 */

#ifndef ALG_BACKEND_RK_RK_INFERER_H
#define ALG_BACKEND_RK_RK_INFERER_H

#include "core/infer/inferer.h"

namespace alg {

class RkInferer : public IInferer {
  public:
    Status Load(const std::string& model_path) override;
    Status Forward() override;
};

}  // namespace alg

#endif  // ALG_BACKEND_RK_RK_INFERER_H
