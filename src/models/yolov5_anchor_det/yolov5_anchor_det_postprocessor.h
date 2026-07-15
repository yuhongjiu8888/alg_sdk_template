/**
 * @file yolov5_anchor_det_postprocessor.h
 * @brief YOLOv5 风格 anchor-based 单尺度检测后处理（SpeedSignNet 用）。
 *
 * 期望网络输出（与 alg_speed_limit/src/deploy_src/postprocess.h 一致）：
 *   - 单尺度，单 head，4D tensor (1, 4+1+num_classes, H, W) 或 (1, H, W, C)
 *   - channel order: [tx, ty, tw, th, obj_logit, cls_logit_0..K-1]
 *
 * 解码（与 alg_speed_limit Python 参考 src/model_src/postprocess.py 完全一致）：
 *   bx = (sigmoid(tx) * 2 - 0.5 + col) * stride
 *   by = (sigmoid(ty) * 2 - 0.5 + row) * stride
 *   bw = (sigmoid(tw) * 2)^2 * anchor_w
 *   bh = (sigmoid(th) * 2)^2 * anchor_h
 *   score = sigmoid(obj) * sigmoid(max(cls_logits))
 *
 * NMS：class-aware（按 label 过滤）。
 *
 * JSON 参数：
 *   { "type": "yolov5_anchor_det",
 *     "num_classes":    1,
 *     "bbox_channels":  4,                         // 可选，默认 4；bbox 通道数
 *     "obj_channels":   1,                         // 可选，默认 1；objectness 通道数
 *     "stride":         8,
 *     "anchor":         [36, 36],
 *     "conf_threshold": 0.25,
 *     "nms_threshold":  0.45,
 *     "max_det":        64,
 *     "obj_prefilter":  0.05
 *   }
 */

#ifndef ALG_MODELS_YOLOV5_ANCHOR_DET_YOLOV5_ANCHOR_DET_POSTPROCESSOR_H
#define ALG_MODELS_YOLOV5_ANCHOR_DET_YOLOV5_ANCHOR_DET_POSTPROCESSOR_H

#include <vector>

#include "core/postprocess/nms.h"
#include "core/postprocess/postprocessor.h"

namespace alg {

class Yolov5AnchorDetPostprocessor : public IPostprocessor {
  public:
    Status Configure(const IInferer& inferer, const Json::Value& params) override;
    Status Apply(const IInferer& inferer, const PreprocessState& state,
                 std::vector<Object>* out) override;

  private:
    int   num_classes_    = 1;
    int   bbox_channels_  = 4;
    int   obj_channels_   = 1;
    int   cls_offset_     = 5;    /* bbox_channels_ + obj_channels_ */
    int   stride_         = 8;
    float anchor_w_       = 36.0f;
    float anchor_h_       = 36.0f;
    float conf_threshold_ = 0.25f;
    float nms_threshold_  = 0.45f;
    int   max_det_        = 64;
    float obj_prefilter_  = 0.05f;
    bool  configured_     = false;

    std::vector<Proposal> props_;
    NmsScratch            nms_scratch_;
};

}  // namespace alg

#endif  // ALG_MODELS_YOLOV5_ANCHOR_DET_YOLOV5_ANCHOR_DET_POSTPROCESSOR_H
