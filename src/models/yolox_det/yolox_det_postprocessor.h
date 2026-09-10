/**
 * @file yolox_det_postprocessor.h
 * @brief mmyolo / mmdet 风格 YOLOXHead 多尺度检测后处理。
 *
 * 期望网络输出布局（与 alg_traffic_light_detection/src/deploy_src/pre_post.cpp 一致）：
 *   - N 个尺度（默认 stride {8, 16, 32}）
 *   - 每个尺度一个 4D tensor，形状 (1, 4+1+num_classes, H, W) 或 (1, H, W, C)
 *   - channel order: [cx_off, cy_off, w_log, h_log, obj_logit, cls_logit_0..K-1]
 *
 * 解码（与 mmdet `YOLOXHead` + `MlvlPointGenerator(offset=0)` 完全一致）：
 *   cx = cx_off * stride + col * stride
 *   cy = cy_off * stride + row * stride
 *   w  = exp(w_log) * stride
 *   h  = exp(h_log) * stride
 *   score = sigmoid(obj_logit) * sigmoid(max(cls_logits))
 *
 * NMS：class-aware（按 label 过滤），与训练侧保持一致。
 *
 * JSON 参数：
 *   { "type": "yolox_det",
 *     "num_classes":    4,
 *     "bbox_channels":  4,                         // 可选，默认 4；bbox 通道数
 *     "obj_channels":   1,                         // 可选，默认 1；objectness 通道数
 *     "strides":        [8, 16, 32],
 *     "conf_threshold": 0.4,
 *     "nms_threshold":  0.5,
 *     "max_det":        100,
 *     "obj_prefilter":  0.05,                      // 早剪枝阈值，省 sigmoid+exp 调用
 *     "class_names":    ["red","yellow","green","off"]
 *   }
 */

#ifndef ALG_MODELS_YOLOX_DET_YOLOX_DET_POSTPROCESSOR_H
#define ALG_MODELS_YOLOX_DET_YOLOX_DET_POSTPROCESSOR_H

#include <string>
#include <vector>

#include "core/postprocess/nms.h"
#include "core/postprocess/postprocessor.h"

namespace alg {

class YoloxDetPostprocessor : public IPostprocessor {
  public:
    Status Configure(const IInferer& inferer, const Json::Value& params) override;
    Status Apply(const IInferer& inferer, const PreprocessState& state,
                 std::vector<Object>* out) override;

  private:
    int                      num_classes_    = 1;
    int                      bbox_channels_  = 4;
    int                      obj_channels_   = 1;
    int                      cls_offset_     = 5;    /* bbox_channels_ + obj_channels_ */
    std::vector<int>         strides_        = {8, 16, 32};
    float                    conf_threshold_ = 0.4f;
    float                    nms_threshold_  = 0.5f;
    int                      max_det_        = 100;
    float                    obj_prefilter_  = 0.05f;
    float                    obj_logit_threshold_ = 0.0f;
    /* class_names 可选；提供时会给每个 Object 加一个 "class" attribute（value_str=类名,
     * value_int=label, value_float=score），方便排错；ABI 层最终走 enum，不依赖此字段。 */
    std::vector<std::string> class_names_;
    std::vector<int>         class_values_;   /* class_names 映射到的业务 enum 值 */
    /* category 可选；提供时再加一个 "category" attribute（value_str=本检测器类别名，
     * 比如 "traffic_light" / "speed_limit"），让应用一眼分辨是哪个检测器的产物。 */
    std::string              category_;
    bool                     configured_     = false;

    /* 复用容器，避免每帧 malloc。 */
    std::vector<Proposal> props_;
    NmsScratch            nms_scratch_;
};

}  // namespace alg

#endif  // ALG_MODELS_YOLOX_DET_YOLOX_DET_POSTPROCESSOR_H
