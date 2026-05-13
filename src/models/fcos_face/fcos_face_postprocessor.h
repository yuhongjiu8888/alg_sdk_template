/**
 * @file fcos_face_postprocessor.h
 * @brief FCOS anchor-free 人脸检测后处理。
 *
 * 与"加新模型"的对照：克隆这个目录 → 改名 → 重写 Apply 解码逻辑 → 在
 * register.cpp 里换一个 REGISTER_ALG_POST 类型字符串即可。
 *
 * 期望的网络输出布局：
 *   - 4 个 FPN 层级，stride 默认 {4, 8, 16, 32}（可通过 JSON 覆写）
 *   - 8 个输出张量：[0..3] 为 cls (1 channel)，[4..7] 为 reg (4 channels: l,t,r,b)
 *   - 张量可为 UINT8 量化或 FP32
 */

#ifndef ALG_MODELS_FCOS_FACE_FCOS_FACE_POSTPROCESSOR_H
#define ALG_MODELS_FCOS_FACE_FCOS_FACE_POSTPROCESSOR_H

#include <vector>

#include "core/postprocess/nms.h"
#include "core/postprocess/postprocessor.h"

namespace alg {

class FcosFacePostprocessor : public IPostprocessor {
  public:
    Status Configure(const IInferer& inferer, const Json::Value& params) override;
    Status Apply(const IInferer& inferer, const PreprocessState& state,
                 std::vector<Object>* out) override;

  private:
    float            conf_threshold_ = 0.6f;
    float            nms_threshold_  = 0.4f;
    int              num_levels_     = 4;
    std::vector<int> strides_        = {4, 8, 16, 32};
    bool             configured_     = false;

    /* 复用：避免每帧重新分配 capacity，clear() 保留容量。 */
    std::vector<Proposal> props_;
    NmsScratch            nms_scratch_;
};

}  // namespace alg

#endif  // ALG_MODELS_FCOS_FACE_FCOS_FACE_POSTPROCESSOR_H
