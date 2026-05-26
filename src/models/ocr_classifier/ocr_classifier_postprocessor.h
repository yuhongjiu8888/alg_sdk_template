/**
 * @file ocr_classifier_postprocessor.h
 * @brief 定长多位字符 OCR 分类器后处理（限速牌 v3.4 SpeedSignOCR 三头识别用）。
 *
 * 替代 dualhead_classifier：双头结构假设"唯一 3 位数是 100"，无法表达 110/120。
 * 本后处理对接逐位读数字的 OCR 网络（训练端 alg_speed_limit/src/classifier_src/
 * classifier.py 的 SpeedSignOCR），按"位"识别再装配成限速值。
 *
 * 期望网络 P 个独立 logits 输出（P = 位数，默认 3：百/十/个位），每个 (1, num_chars)：
 *   - logits_h  (1, 11)  —— 百位字符，char ∈ {'0'..'9', blank}
 *   - logits_t  (1, 11)  —— 十位字符
 *   - logits_u  (1, 11)  —— 个位字符
 * 字符表：idx 0..9 = '0'..'9'，idx blank_index(默认 10) = 占位/无此位。
 *
 * 三头**同形状 (1,11)，无法靠 size 区分**（不同于 dualhead 的 8 vs 2）。因此本后处理
 * 优先按输出张量 name 匹配（head_names），name 不可用（如 XMM 输出名为空）时退回
 * 配置的固定索引 head_indices。
 *
 * 解码（与 classifier.py 的 CLASS_TO_CHARS / build_decode_lut 等价，但**不硬编码**，
 * 完全由 class_names 推导）：
 *   - 每个 class_name 是限速字符串（"10".."120"），右对齐拆成 P 位字符，缺位补 blank；
 *     例 "90"→(blank,'9','0')，"120"→('1','2','0')，"100"→('1','0','0')。
 *   - 由此构建 (num_chars^P) 的解码 LUT：合法组合 → class idx，其余 → -1 拒识。
 *   - 推理时三头各 softmax+argmax 得 (h,t,u)，查 LUT 得 cls_id；非法组合（含个位非 '0'）
 *     → 拒识（返回空 → ChainSolution `classify_into:` 触发 src 框 drop）。
 *   - 置信度 cls_conf = min(三头各自 max softmax prob)（最不确定那位当门槛，与
 *     alg_speed_limit/src/infer_onnx.py 一致）；< conf_threshold → 丢弃（开放集兜底）。
 *
 * Object 输出（成功时）：
 *   - field_mask = ALG_FIELD_BOX | ALG_FIELD_ATTRIBUTES
 *   - label = cls_id（class_names 下标），box.score = cls_conf
 *   - value = class_names[cls_id] 解析出的限速值 ÷ 10（即 C ABI 的 AlgSpeedLimitValue，
 *             SLV_10=1..SLV_120=12；FillAlgResult 直接读取，不再硬编码映射表）
 *   - attributes[0] = { name="class", value_int=cls_id, value_str=class_name, value_float=cls_conf }
 *   - 若配置了 category，再追加 { name="category", value_str=category }
 *
 * JSON 参数：
 *   { "type": "ocr_classifier",
 *     "category":       "speed_limit",
 *     "num_positions":  3,                              // 位数 = 头数；默认按 class_names 最长位数推导
 *     "num_chars":      11,                             // 每头类别数（'0'..'9' + blank）
 *     "blank_index":    10,                             // 占位符在字符表中的下标
 *     "head_names":     ["logits_h","logits_t","logits_u"],  // 按名匹配（高位→低位）
 *     "head_indices":   [0, 1, 2],                      // name 不可用时的索引兜底（高位→低位）
 *     "conf_threshold": 0.5,                            // min(三头 prob) 低于此值 → drop
 *     "class_names":    ["10","20","30","40","50","60","70","80","100","90","110","120"]
 *   }
 */

#ifndef ALG_MODELS_OCR_CLASSIFIER_OCR_CLASSIFIER_POSTPROCESSOR_H
#define ALG_MODELS_OCR_CLASSIFIER_OCR_CLASSIFIER_POSTPROCESSOR_H

#include <string>
#include <vector>

#include "core/postprocess/postprocessor.h"

namespace alg {

class OcrClassifierPostprocessor : public IPostprocessor {
  public:
    Status Configure(const IInferer& inferer, const Json::Value& params) override;
    Status Apply(const IInferer& inferer, const PreprocessState& state,
                 std::vector<Object>* out) override;

  private:
    int num_positions_ = 3;   /* 位数 = 头数（百/十/个） */
    int num_chars_     = 11;  /* 每头类别数：'0'..'9' + blank */
    int blank_index_   = 10;  /* 占位符下标 */
    float conf_threshold_ = 0.5f;

    std::vector<int>         resolved_head_idx_;  /* 各位对应的 output tensor 索引（高位→低位） */
    std::vector<std::string> class_names_;
    std::vector<int>         value_lut_;          /* class idx → AlgSpeedLimitValue（km/h÷10） */
    std::vector<int>         decode_lut_;         /* 扁平 (num_chars^P) → class idx；非法=-1 */

    std::string category_;
    bool        configured_ = false;
};

}  // namespace alg

#endif  // ALG_MODELS_OCR_CLASSIFIER_OCR_CLASSIFIER_POSTPROCESSOR_H
