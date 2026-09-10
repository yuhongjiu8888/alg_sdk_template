/**
 * @file rtmdet_det_postprocessor.h
 * @brief RTMDet 多尺度单类检测后处理（车牌检测 license_detection 用）。
 *
 * 期望网络输出（与 license_bralizera 转换配置一致）：
 *   - 6 个 4D head：cls_score_s8/s16/s32 (1, 1, H, W) + bbox_pred_s8/s16/s32 (1, 4, H, W)
 *   - H = input_h / stride，stride ∈ {8, 16, 32}；reg 已做 DFL integral 并乘过 stride
 *
 * 解码（与 license demo 的 decode_rtmdet / demo.py 严格一致）：
 *   - prior = (grid + 0) * stride
 *   - 框 = [prior_x - l, prior_y - t, prior_x + r, prior_y + b]
 *   - score = sigmoid(cls)
 *   - clip 到模型输入空间 → 过滤 min_bbox_size → 单类 NMS → 按分数截断 max_det
 *   - 坐标经 PreprocessState 反映射回原图（与 yolox_det 一致，比 demo 的近似
 *     scale_x/scale_y 更精确）
 *
 * JSON 参数：
 *   { "type": "rtmdet_det",
 *     "num_classes":    1,       // cls 通道数（默认 1，车牌单类）
 *     "bbox_channels":  4,       // reg 通道数（默认 4）
 *     "strides":        [8,16,32],
 *     "input_size":     [640,448], // 模型输入 [W,H]，用于推导 head 的 stride
 *     "conf_threshold": 0.25,
 *     "nms_threshold":  0.45,
 *     "min_bbox_size":  4.0,
 *     "max_det":        2,
 *     "category":       "license_plate" }  // 可选：单阶段直出时打 category（二阶段由识别器覆盖）
 */

#ifndef ALG_MODELS_RTMDET_DET_RTMDET_DET_POSTPROCESSOR_H
#define ALG_MODELS_RTMDET_DET_RTMDET_DET_POSTPROCESSOR_H

#include <vector>

#include "core/postprocess/nms.h"
#include "core/postprocess/postprocessor.h"

namespace alg {

class RtmdetDetPostprocessor : public IPostprocessor {
  public:
    Status Configure(const IInferer& inferer, const Json::Value& params) override;
    Status Apply(const IInferer& inferer, const PreprocessState& state,
                 std::vector<Object>* out) override;

  private:
    int   num_classes_    = 1;
    int   bbox_channels_  = 4;
    float conf_threshold_ = 0.25f;
    float nms_threshold_  = 0.45f;
    float min_bbox_size_  = 4.0f;
    int   max_det_        = 2;
    float conf_logit_threshold_ = 0.0f;
    std::string category_;
    bool  configured_     = false;

    int input_w_ = 0;   /* 模型输入宽（Configure 时从 inferer.InputView(0) 读） */
    int input_h_ = 0;

    std::vector<int> strides_;       /* level → stride（默认 8/16/32） */
    std::vector<int> cls_idx_;       /* level → cls 输出张量索引 */
    std::vector<int> reg_idx_;       /* level → reg 输出张量索引 */

    std::vector<Proposal> props_;
    std::vector<Proposal> filtered_;
    NmsScratch            nms_scratch_;
};

}  // namespace alg

#endif  // ALG_MODELS_RTMDET_DET_RTMDET_DET_POSTPROCESSOR_H
