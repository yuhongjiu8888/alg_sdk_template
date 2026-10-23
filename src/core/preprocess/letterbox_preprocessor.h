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
#include "core/solution/crop_util.h"

namespace alg {

class LetterboxPreprocessor : public IPreprocessor {
  public:
    Status Configure(const PreprocessConfig& cfg, const TensorView& input) override;
    Status Apply(const AlgImage& image, TensorView& input, PreprocessState& state) override;

    /* 融合路径：直接从 NV12/NV21 原图的 ROI 生成模型输入。
     * 先在 NV12 域缩放到目标尺寸，再做一次小图转色，跳过“裁剪图转 BGR +
     * 大图 resize + BGR→RGB”两遍全尺寸处理。crop_cfg 决定 ROI 扩边/填充。 */
    Status ApplyNv12Roi(const AlgImage& image,
                        const AlgBox& box,
                        const CropConfig& crop_cfg,
                        TensorView& input,
                        PreprocessState& state,
                        CropTransform* xf);

  private:
    PreprocessConfig cfg_{};
    int net_w_ = 0;
    int net_h_ = 0;
    bool configured_ = false;
    bool need_norm_ = false;    /* 在 Configure 一次性算出，避免每帧判断 */
    bool u8_fast_path_ = false; /* U8 且无归一化：可直接在 WriteToTensor 交换 R/B */

    /* 复用的中间缓存，避免每帧分配。 */
    cv::Mat resized_;
    cv::Mat y_resized_;
    cv::Mat uv_resized_;
    cv::Mat fused_y_;   /* ApplyNv12Roi：目标尺寸的 Y 平面 */
    cv::Mat fused_uv_;  /* ApplyNv12Roi：目标尺寸的 UV 平面 */
    cv::Mat fused_rgb_; /* ApplyNv12Roi：padded 情况下有效区域的连续 RGB 缓冲 */

    /* DecodeAndResize 保持源通道顺序、WriteToTensor 再交换 R/B，省一次 cvtColor。 */
    bool swap_rb_ = false;

    /* 由 resize 模式 + 源尺寸计算 scale / 目标活动区 / letterbox pad。 */
    void ComputeGeometry(
        int src_w, int src_h, float* scale, int* new_w, int* new_h, int* pad_left, int* pad_top) const;

    Status DecodeAndResize(const AlgImage& image, int new_w, int new_h);
    Status WriteToTensor(const TensorView& input, int new_w, int new_h, int pad_left, int pad_top);

    /* 只 memset padding 区域，活动区不动（节省一大块 memset）。 */
    void FillPaddingU8(uint8_t* plane, int new_w, int new_h, int pad_left, int pad_top) const;
    void FillPaddingF32(float* plane, int new_w, int new_h, int pad_left, int pad_top) const;
};

}  // namespace alg

#endif  // ALG_CORE_PREPROCESS_LETTERBOX_PREPROCESSOR_H
