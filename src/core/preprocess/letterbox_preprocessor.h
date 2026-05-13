/**
 * @file letterbox_preprocessor.h
 * @brief Aspect-ratio-preserving resize + pad + format conversion.
 *
 * Implements the same path as sgk-sdk's Preprocess(): for NV12/NV21
 * inputs it resizes Y and UV planes BEFORE cvtColor so the heavy
 * colorspace conversion only touches the downscaled image. NEON
 * vld3/vst1 splits BGR-interleaved to planar BGR for NCHW.
 *
 * This implementation is chip-agnostic. The output buffer it writes
 * into may be MMZ (XMM), DMA-buf (RK), or plain malloc.
 */

#ifndef ALG_CORE_PREPROCESS_LETTERBOX_PREPROCESSOR_H
#define ALG_CORE_PREPROCESS_LETTERBOX_PREPROCESSOR_H

#include <opencv2/core/mat.hpp>

#include "core/preprocess/preprocessor.h"

namespace alg {

class LetterboxPreprocessor : public IPreprocessor {
  public:
    Status Configure(const PreprocessConfig& cfg, const TensorView& input) override;
    Status Apply(const AlgImage& image, TensorView& input, PreprocessState& state) override;

  private:
    PreprocessConfig cfg_{};
    int   net_w_ = 0;
    int   net_h_ = 0;
    bool  configured_ = false;
    bool  need_norm_  = false;   /* 在 Configure 一次性算出，避免每帧判断 */

    /* 复用的中间缓存，避免每帧分配。 */
    cv::Mat resized_;
    cv::Mat y_resized_;
    cv::Mat uv_resized_;

    Status DecodeAndResize(const AlgImage& image, int new_w, int new_h);
    Status WriteToTensor(const TensorView& input, int new_w, int new_h, int pad_left, int pad_top);

    /* 只 memset padding 区域，活动区不动（节省一大块 memset）。 */
    void FillPaddingU8(uint8_t* plane, int new_w, int new_h, int pad_left, int pad_top) const;
    void FillPaddingF32(float* plane, int new_w, int new_h, int pad_left, int pad_top) const;
};

}  // namespace alg

#endif  // ALG_CORE_PREPROCESS_LETTERBOX_PREPROCESSOR_H
