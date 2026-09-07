/**
 * @file preprocessor.h
 * @brief Generic preprocessor interface.
 *
 * A preprocessor is fully described by `PreprocessConfig` data, so a
 * single implementation (LetterboxPreprocessor) can serve many models.
 * Models customize behavior through config, not through subclassing.
 */

#ifndef ALG_CORE_PREPROCESS_PREPROCESSOR_H
#define ALG_CORE_PREPROCESS_PREPROCESSOR_H

#include "alg_types.h"
#include "core/status.h"
#include "core/tensor.h"

namespace alg {

enum class ColorOrder : int { kBGR, kRGB, kGray };

enum class ResizeMode : int {
    kStretch,       /* resize directly to (W, H), aspect-ratio distortion */
    kLetterboxTL,   /* keep aspect ratio, pad to top-left (matches sgk-sdk FCOS) */
    kLetterboxTLFit,/* keep aspect ratio, fit both dims (min-scale), pad top-left
                       —— 车牌 RTMDet 640x448 demo 的 _resize_pad_top_left */
    kLetterboxCenter
};

/**
 * Per-model preprocessing parameters. All fields are data; the same
 * `LetterboxPreprocessor` implementation honors them.
 *
 * Normalization recipe (applied to each pixel):
 *     y = (x - mean) * (scale * 1/std)
 * For uint8 inputs with no normalization, leave mean=0, scale=1, std=1.
 */
struct PreprocessConfig {
    int        net_width = 0;
    int        net_height = 0;
    ColorOrder color = ColorOrder::kBGR;
    ResizeMode resize = ResizeMode::kLetterboxTL;
    Layout     layout = Layout::kNCHW;
    float      mean[3] = {0, 0, 0};
    float      std[3]  = {1, 1, 1};
    float      scale = 1.0f;
    uint8_t    pad_value = 0;
};

/**
 * Per-frame state shared between preprocess and postprocess.
 *
 * Letterbox scale/offset is the only thing post needs to recover
 * original-image coordinates. Decoupled from any specific image source.
 */
struct PreprocessState {
    float scale_ratio = 1.0f;   /* ratio applied to the original image */
    int   pad_left = 0;
    int   pad_top  = 0;
    int   original_width = 0;
    int   original_height = 0;
};

class IPreprocessor {
  public:
    virtual ~IPreprocessor() = default;

    /** Validate/cache against the inferer's input tensor descriptor. */
    virtual Status Configure(const PreprocessConfig& cfg, const TensorView& input) = 0;

    /**
     * Write preprocessed bytes directly into `input.data`. `state` is
     * filled with the geometric transform applied so the postprocessor
     * can invert it.
     */
    virtual Status Apply(const AlgImage& image, TensorView& input, PreprocessState& state) = 0;
};

}  // namespace alg

#endif  // ALG_CORE_PREPROCESS_PREPROCESSOR_H
