/**
 * @file mnn_inferer.h
 * @brief IInferer implementation using MNN for local CPU/GPU inference.
 *
 * Purpose: local model verification before deploying to the target NPU.
 * Uses the same TensorView contract as chip backends, so all pre/post
 * processing code works unchanged.
 */

#ifndef ALG_BACKEND_MNN_MNN_INFERER_H
#define ALG_BACKEND_MNN_MNN_INFERER_H

#include "core/infer/inferer.h"

#include <MNN/Interpreter.hpp>
#include <MNN/MNNForwardType.h>
#include <MNN/Tensor.hpp>

namespace alg {

class MnnInferer : public IInferer {
  public:
    MnnInferer();
    ~MnnInferer() override;

    Status Load(const std::string& model_path) override;
    Status Forward() override;

  private:
    void FreeAll();
    Status BuildViews();

    MNN::Interpreter* interpreter_ = nullptr;
    MNN::Session*     session_     = nullptr;
    bool              initialized_ = false;
};

}  // namespace alg

#endif  // ALG_BACKEND_MNN_MNN_INFERER_H
