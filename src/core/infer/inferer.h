/**
 * @file inferer.h
 * @brief Chip-agnostic forward-inference interface.
 *
 * THIS IS THE ONE INTERFACE THAT YOU REIMPLEMENT TO PORT TO A NEW CHIP.
 *
 * Contract:
 *   1. Load(): parse model file, allocate chip-specific I/O memory, fill
 *      in `input_views_` and `output_views_` so they point at host-visible
 *      buffers with correct shape/dtype/quant metadata.
 *   2. Forward(): caller has filled InputView(i).data with preprocessed
 *      bytes; backend syncs/flushes as needed, runs NPU graph, ensures
 *      OutputView(i).data is readable from host on return.
 *   3. The pre/post layers only see TensorView; they have no idea what
 *      kind of memory it is.
 */

#ifndef ALG_CORE_INFER_INFERER_H
#define ALG_CORE_INFER_INFERER_H

#include <string>
#include <vector>

#include "core/status.h"
#include "core/tensor.h"
#include "core/preprocess/preprocessor.h"

namespace alg {

class IInferer {
  public:
    virtual ~IInferer() = default;

    /** Load the model. After success, InputView()/OutputView() are usable. */
    virtual Status Load(const std::string& model_path) = 0;

    /** Run one forward pass against the currently-populated input views. */
    virtual Status Forward() = 0;

    /** 可选的芯片动态图像前处理；默认后端不支持。 */
    virtual bool SupportsDynamicAipp() const { return false; }
    virtual Status PrepareDynamicAipp(const AlgImage&,
                                      const PreprocessConfig&,
                                      PreprocessState&) {
        return ALG_E_BACKEND;
    }

    int NumInputs() const { return static_cast<int>(input_views_.size()); }
    int NumOutputs() const { return static_cast<int>(output_views_.size()); }

    /** Mutable so preprocessor can write into data buffer. */
    TensorView& InputView(int i) { return input_views_[i]; }
    /** Const for postprocessor — data is read-only after Forward(). */
    const TensorView& OutputView(int i) const { return output_views_[i]; }

  protected:
    std::vector<TensorView> input_views_;
    std::vector<TensorView> output_views_;
};

}  // namespace alg

#endif  // ALG_CORE_INFER_INFERER_H
