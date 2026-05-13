/**
 * @file dualhead_classifier_postprocessor.h
 * @brief 双头数字识别分类器后处理（限速牌 SpeedSignClassifier 用）。
 *
 * 期望网络两个独立 logits 输出（与 alg_speed_limit/src/deploy_src/postprocess.h 一致）：
 *   - head_a: shape (1, A)  —— 首位数字 (默认 8 类: 1..8)
 *   - head_b: shape (1, B)  —— 是否 N 位数 (默认 2 类: 否 / 是)
 *
 * 两头独立 softmax + 装配规则（默认匹配巴西限速牌 9 类 10/20/30/40/50/60/70/80/100）：
 *   if argmax(head_b) == is_3digit_class:
 *       cls_id = three_digit_class_index   (对应 "100")
 *   else:
 *       cls_id = argmax(head_a)            (0..A-1 对应 "10".."80")
 *   joint_conf = max(softmax(head_a)) * max(softmax(head_b))
 *
 * 当 joint_conf < conf_threshold 时返回空（不产出 Object）。当上游 ChainSolution
 * 用 `produces:"classify_into:<stage>"` 接入时，空产出会触发 source 框 drop。
 *
 * Object 输出（成功时）：
 *   - field_mask = ALG_FIELD_BOX | ALG_FIELD_ATTRIBUTES
 *   - box.label = cls_id, box.score = joint_conf  （ChainSolution 会用这两项更新 src.box）
 *   - attributes[0] = { name="class", value_int=cls_id, value_str=class_name,
 *                       value_float=joint_conf }
 *
 * JSON 参数：
 *   { "type": "dualhead_classifier",
 *     "head_a_index":         0,
 *     "head_b_index":         1,
 *     "head_a_classes":       8,             // 首位数字头长度
 *     "head_b_classes":       2,             // 第二头长度
 *     "is_3digit_class":      1,             // head_b argmax == 此值时归为 3 位数类
 *     "three_digit_class_index": 8,          // 3 位数对应的最终 cls_id（"100" 在 9 类里的 idx）
 *     "conf_threshold":       0.7,           // joint_conf 低于此值 → drop
 *     "class_names":          ["10","20","30","40","50","60","70","80","100"]
 *   }
 */

#ifndef ALG_MODELS_DUALHEAD_CLASSIFIER_DUALHEAD_CLASSIFIER_POSTPROCESSOR_H
#define ALG_MODELS_DUALHEAD_CLASSIFIER_DUALHEAD_CLASSIFIER_POSTPROCESSOR_H

#include <string>
#include <vector>

#include "core/postprocess/postprocessor.h"

namespace alg {

class DualheadClassifierPostprocessor : public IPostprocessor {
  public:
    Status Configure(const IInferer& inferer, const Json::Value& params) override;
    Status Apply(const IInferer& inferer, const PreprocessState& state,
                 std::vector<Object>* out) override;

  private:
    int                      head_a_index_       = 0;
    int                      head_b_index_       = 1;
    int                      head_a_classes_     = 8;
    int                      head_b_classes_     = 2;
    int                      is_3digit_class_    = 1;
    int                      three_digit_idx_    = 8;
    float                    conf_threshold_     = 0.7f;
    std::vector<std::string> class_names_;
    /* category 可选；同 yolox_det 用法，便于多检测器并行 solution 里区分本框是哪个检测器产物。 */
    std::string              category_;
    bool                     configured_         = false;
};

}  // namespace alg

#endif  // ALG_MODELS_DUALHEAD_CLASSIFIER_DUALHEAD_CLASSIFIER_POSTPROCESSOR_H
