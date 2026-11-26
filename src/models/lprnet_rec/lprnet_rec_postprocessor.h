/**
 * @file lprnet_rec_postprocessor.h
 * @brief LPRNet CTC 车牌识别后处理（license_recognizer 用）。
 *
 * 期望网络输出：`1x32x37`（32 时间步 × 37 类：'0'-'9','A'-'Z' 36 字符 + blank），
 * 内存按时间步主序 [t][c] 连续（与 license demo 的 unpack_to_float(rows=32, cols=37)
 * 读取一致）。输出通常是 FP16（INT8 量化模型 ATC 映射后 head 为 FP16 计算）。
 *
 * 解码：逐时间步 softmax → argmax → 合并连续重复、去 blank。
 * 字符分数取保留该字符时的时间步概率，不合并后续重复时间步的分数。
 *
 * Object 输出：
 *   - field_mask = ALG_FIELD_ATTRIBUTES
 *   - attributes = [{category=category}, {text=识别文本}, {char_scores=字符分数}]
 *   使用 attributes_into 保留检测分数；空文本、长度和字符分数由上层业务过滤。
 *
 * JSON 参数：
 *   { "type": "lprnet_rec",
 *     "category":      "license_plate",
 *     "characters":    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ",
 *     "blank_index":   36,
 *     "timesteps":     32,
 *     "channels":      37 }   // 可选，默认 = characters.size() + 1
 */

#ifndef ALG_MODELS_LPRNET_REC_LPRNET_REC_POSTPROCESSOR_H
#define ALG_MODELS_LPRNET_REC_LPRNET_REC_POSTPROCESSOR_H

#include <string>
#include <vector>
#include "core/postprocess/postprocessor.h"

namespace alg {

class LprnetRecPostprocessor : public IPostprocessor {
  public:
    Status Configure(const IInferer& inferer, const Json::Value& params) override;
    Status Apply(const IInferer& inferer, const PreprocessState& state, std::vector<Object>* out) override;

  private:
    std::string characters_; /* 字符表（不含 blank），如 "0..9A..Z" */
    int blank_index_ = 36;
    int timesteps_ = 32;
    int channels_ = 37; /* 类别数（含 blank） */
    std::string category_;
    bool configured_ = false;
};

}  // namespace alg

#endif  // ALG_MODELS_LPRNET_REC_LPRNET_REC_POSTPROCESSOR_H
