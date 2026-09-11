/**
 * @file lprnet_rec_postprocessor.h
 * @brief LPRNet CTC 车牌识别后处理（license_recognizer 用）。
 *
 * 期望网络输出：`1x32x37`（32 时间步 × 37 类：'0'-'9','A'-'Z' 36 字符 + blank），
 * 内存按时间步主序 [t][c] 连续（与 license demo 的 unpack_to_float(rows=32, cols=37)
 * 读取一致）。输出通常是 FP16（INT8 量化模型 ATC 映射后 head 为 FP16 计算）。
 *
 * 解码（与 license demo 的 ctc_greedy_decode 严格一致）：
 *   逐时间步 softmax → argmax → 合并连续重复、去 blank，识别分数为被保留字符的
 *   softmax 概率之积。
 *
 * Object 输出（识别成功时）：
 *   - field_mask = ALG_FIELD_BOX | ALG_FIELD_ATTRIBUTES
 *   - box.score = rec_score（classify_into 阶段会再乘 det 作联合置信度）
 *   - attributes = [{category=category}, {text=识别文本}, {rec_score=...}]
 *   FillAlgResult 按 category=="license_plate" 分桶，读 text / rec_score 属性。
 *   空文本始终返回空；conf_threshold > 0 时识别分低于阈值也返回空
 *   （ChainSolution classify_into 触发 drop）。
 *
 * JSON 参数：
 *   { "type": "lprnet_rec",
 *     "category":      "license_plate",
 *     "characters":    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ",
 *     "blank_index":   36,
 *     "timesteps":     32,
 *     "channels":      37,     // 可选，默认 = characters.size() + 1
 *     "conf_threshold": 0.0 }  // >0：识别分低于此值 drop；0（默认）= 不过滤（对齐 demo）
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
    Status Apply(const IInferer& inferer, const PreprocessState& state,
                 std::vector<Object>* out) override;

  private:
    std::string characters_;     /* 字符表（不含 blank），如 "0..9A..Z" */
    int   blank_index_ = 36;
    int   timesteps_   = 32;
    int   channels_    = 37;     /* 类别数（含 blank） */
    float conf_threshold_ = 0.0f;
    std::string category_;
    bool  configured_  = false;
};

}  // namespace alg

#endif  // ALG_MODELS_LPRNET_REC_LPRNET_REC_POSTPROCESSOR_H
