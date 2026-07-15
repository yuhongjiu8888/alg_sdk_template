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
 * v3.5 门控（3 路 other/speed/no_parking，gate_channels>0 时启用；单输出=OCR 段之后 P*num_chars，
 * 多输出=独立 logits_gate 头）。门控 argmax 定牌种：
 *   - speed（且 P(限速)≥gate_threshold）→ 走上面 OCR，输出限速值（category=category）
 *   - no_parking → 不读 OCR，直接产出禁停正类：category=nopark_category、value=nopark_sign_value
 *     （AlgSignType SIGN_NO_PARKING）、box.score=P(禁停)。FillAlgResult 据 category 分桶到 signs[]。
 *     nopark_min_size（最小框短边）在 ChainSolution 的 min_box_short 施加（此处拿不到原图框尺寸）。
 *   - other（或 P(限速)<gate_threshold）→ 拒识（返回空 → classify_into drop）
 *
 * Object 输出（成功时）：
 *   - field_mask = ALG_FIELD_BOX | ALG_FIELD_ATTRIBUTES
 *   - 限速：label=cls_id、box.score=cls_conf、value=限速值÷10（AlgSpeedLimitValue SLV_10..SLV_120）、
 *     attributes=[{class,...},{category=category}]
 *   - no_parking：label=-1、box.score=P(禁停)、value=nopark_sign_value、attributes=[{category=nopark_category}]
 *   FillAlgResult 直接按 category 分桶 + 读 value，不再硬编码映射表。
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
 *     "gate_channels":  3,                              // v3.5 门控路数；0=无门控纯 OCR
 *     "gate_threshold": 0.5,                            // P(限速) < 此值 → 拒识
 *     "gate_head_name": "logits_gate",                  // 仅多输出：门控头名
 *     "nopark_category":"no_parking",                   // no_parking 正类 category（分桶 signs[]）
 *     "nopark_sign_value": 1,                           // AlgSignType SIGN_NO_PARKING
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

    /* 单输出模式：模型只出一个 (1, P*C) / (1,P,C) 张量（避开 XMM 多输出+CPU 切图
     * 的板端坑），各位在该张量内按 [p*num_chars, p*num_chars+num_chars) 连续切片。
     * NumOutputs==1 且 P>1 时自动启用；NumOutputs>=P 时仍走原多输出头逻辑。 */
    bool single_output_ = false;
    std::vector<int>         resolved_head_idx_;  /* 各位对应的 output tensor 索引（高位→低位） */
    std::vector<std::string> class_names_;
    std::vector<int>         value_lut_;          /* class idx → AlgSpeedLimitValue（km/h÷10） */
    std::vector<int>         decode_lut_;         /* 扁平 (num_chars^P) → class idx；非法=-1 */

    std::string category_;

    /* v3.5 门控（3 路 other/speed/no_parking）。gate_channels_==0 → 无门控（纯 OCR，向后兼容）。
     * 单输出模式：门控在 output[0] 内，偏移 gate_base_=P*num_chars；
     * 多输出模式：门控是独立头（gate_head_name/gate_head_index），gate_base_=0。 */
    int   gate_channels_    = 0;
    int   gate_speed_index_ = 1;   /* P(限速) 在门控向量里的下标 */
    int   gate_nopark_index_ = 2;  /* P(禁停) 在门控向量里的下标 */
    float gate_threshold_   = 0.5f;
    int   gate_view_idx_    = -1;  /* 门控所在 output tensor 索引；<0 = 未解析/无门控 */
    int   gate_base_        = 0;   /* 门控在该 tensor 内的起始元素偏移 */
    std::string nopark_category_;  /* no_parking 正类产出的 category（FillAlgResult 据此分桶到 signs[]） */
    int         nopark_sign_value_ = 0;  /* AlgSignType（SIGN_NO_PARKING=1） */

    bool        configured_ = false;
};

}  // namespace alg

#endif  // ALG_MODELS_OCR_CLASSIFIER_OCR_CLASSIFIER_POSTPROCESSOR_H
