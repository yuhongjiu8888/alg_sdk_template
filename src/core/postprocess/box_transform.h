/** Restore detection boxes to original-image coordinates. */
#ifndef ALG_CORE_POSTPROCESS_BOX_TRANSFORM_H
#define ALG_CORE_POSTPROCESS_BOX_TRANSFORM_H

#include "alg_types.h"
#include "core/postprocess/nms.h"
#include "core/preprocess/preprocessor.h"

namespace alg {

/* 每帧构造一次，缓存反缩放系数，供该帧全部候选框复用。 */
class DetectionBoxTransform {
  public:
    explicit DetectionBoxTransform(const PreprocessState& state)
        : pad_left_(state.pad_left),
          pad_top_(state.pad_top),
          max_x_(state.original_width - 1),
          max_y_(state.original_height - 1) {
        const float inv_s = state.scale_ratio > 0 ? 1.0f / state.scale_ratio : 1.0f;
        inv_x_ = state.scale_x > 0 ? 1.0f / state.scale_x : inv_s;
        inv_y_ = state.scale_y > 0 ? 1.0f / state.scale_y : inv_s;
    }

    AlgBox Apply(const Proposal& p) const {
        AlgBox box{};
        box.score = p.score;
        box.xmin = MapCoordinate(p.x1, pad_left_, inv_x_, max_x_);
        box.ymin = MapCoordinate(p.y1, pad_top_, inv_y_, max_y_);
        box.xmax = MapCoordinate(p.x2, pad_left_, inv_x_, max_x_);
        box.ymax = MapCoordinate(p.y2, pad_top_, inv_y_, max_y_);
        return box;
    }

  private:
    static int MapCoordinate(float value, int pad, float inv_scale, int max_value) {
        /* 保留原有 +0.5f 后截断及裁边顺序。 */
        const int coord = static_cast<int>((value - pad) * inv_scale + 0.5f);
        return coord < 0 ? 0 : (coord > max_value ? max_value : coord);
    }

    int pad_left_, pad_top_;
    int max_x_, max_y_;
    float inv_x_, inv_y_;
};

}  // namespace alg

#endif  // ALG_CORE_POSTPROCESS_BOX_TRANSFORM_H
