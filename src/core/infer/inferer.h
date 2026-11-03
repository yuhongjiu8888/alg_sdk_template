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
 *      bytes, or used an optional hardware image-preparation hook below;
 *      backend syncs/flushes as needed, runs NPU graph, ensures
 *      OutputView(i).data is readable from host on return.
 *   3. The pre/post layers only see TensorView; they have no idea what
 *      kind of memory it is.
 */

#ifndef ALG_CORE_INFER_INFERER_H
#define ALG_CORE_INFER_INFERER_H

#include <cstddef>
#include <string>
#include <vector>
#include "core/preprocess/preprocessor.h"
#include "core/status.h"
#include "core/tensor.h"

namespace alg {

struct CropConfig;
struct CropTransform;

/**
 * 同一条 ChainSolution 内多个硬件前处理模型共享的 YUV staging 描述。
 *
 * 未提供物理地址的分平面测试输入由 libyuv 按原尺寸合并，后续模型在源平面
 * 和原图 staging 布局相同时临时绑定复用。生产环境的物理双平面输入走 IVE，
 * 连续 MMZ/VB 映射直接绑定，均不使用本结构。描述本身不拥有内存，实际
 * staging 缓冲仍由第一个 inferer 持有。
 * 亿智后端在普通内存输入时发布已刷新缓存的设备缓冲物理地址，后续整帧和 ROI
 * 模型均可直接读取。每次 AlgRun 开始清空描述，避免复用上一帧内容。
 */
struct SharedInputStaging {
    const void* source_plane[2] = {};
    int source_width = 0;
    int source_height = 0;
    int source_stride[2] = {};
    AlgPixelFormat source_format = ALG_PIX_BGR;
    int source_data_len[2] = {};
    bool source_uses_separate_planes = false;

    int staged_width = 0;
    int staged_height = 0;

    void* data = nullptr;
    size_t capacity = 0;
    size_t data_size = 0;
    size_t row_stride = 0;
    bool flushed = false;
    /* EZ image-engine staging; zero for backends without physical device buffers. */
    uint64_t device_phys_addr = 0;

    /* DV500 CPU 预缩放的 1/2 折半中间帧（如 1080p 的 960x540）。第一个全图
     * 模型产出后发布，后续模型中间帧尺寸相同时可直接复用，避免重复折半。
     * 只由 CPU 读写，不绑定 ACL；每帧 Reset。 */
    void* prescale_data = nullptr;
    int prescale_width = 0;
    int prescale_height = 0;
    int prescale_stride = 0;

    void Reset() { *this = SharedInputStaging{}; }
};

class IInferer {
  public:
    virtual ~IInferer() = default;

    /** Load the model. After success, InputView()/OutputView() are usable. */
    virtual Status Load(const std::string& model_path) = 0;

    /** Run one forward pass against the currently-populated input views. */
    virtual Status Forward() = 0;

    /** Reject unsupported static preprocessing settings during model initialization.
     * AIPP is HiSilicon-specific; its backend explicitly enables that engine. */
    virtual Status ValidatePreprocessConfig(const PreprocessConfig& cfg) const {
        return cfg.engine == PreprocessEngine::kAipp ? ALG_E_CONFIG : ALG_OK;
    }

    /** Optional backend hardware image preprocessing; unavailable by default. */
    virtual bool SupportsHardwarePreprocess() const { return false; }
    virtual Status PrepareHardwareInput(const AlgImage&, const PreprocessConfig&, PreprocessState&, SharedInputStaging*) {
        return ALG_E_BACKEND;
    }

    /** Optional hardware crop/resize directly from the original NV12/NV21 frame. */
    virtual bool SupportsNv12Roi() const { return false; }
    virtual Status PrepareNv12Roi(const AlgImage&, const AlgBox&, const CropConfig&, const PreprocessConfig&,
                                  PreprocessState&, CropTransform*, SharedInputStaging*) {
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
