#include "models/dualhead_classifier/dualhead_classifier_postprocessor.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/logger.h"

namespace alg {

namespace {

inline float ReadElem(const TensorView& t, int idx) {
    switch (t.dtype) {
        case DataType::kF32: return static_cast<const float*>(t.data)[idx];
        case DataType::kI8: {
            int raw = static_cast<int>(static_cast<const int8_t*>(t.data)[idx]);
            return (raw - t.quant.zero_point) * t.quant.scale;
        }
        case DataType::kU8: {
            int raw = static_cast<int>(static_cast<const uint8_t*>(t.data)[idx]);
            return (raw - t.quant.zero_point) * t.quant.scale;
        }
        case DataType::kI16: {
            int raw = static_cast<int>(static_cast<const int16_t*>(t.data)[idx]);
            return (raw - t.quant.zero_point) * t.quant.scale;
        }
        case DataType::kU16: {
            int raw = static_cast<int>(static_cast<const uint16_t*>(t.data)[idx]);
            return (raw - t.quant.zero_point) * t.quant.scale;
        }
        case DataType::kI32: {
            int raw = static_cast<const int32_t*>(t.data)[idx];
            return (raw - t.quant.zero_point) * t.quant.scale;
        }
        case DataType::kF16: {
            uint16_t h = static_cast<const uint16_t*>(t.data)[idx];
            uint32_t sign = (h & 0x8000u) << 16;
            uint32_t exp  = (h & 0x7C00u) >> 10;
            uint32_t mant = (h & 0x03FFu);
            uint32_t f;
            if (exp == 0) {
                if (mant == 0) { f = sign; }
                else {
                    exp = 1;
                    while ((mant & 0x0400u) == 0) { mant <<= 1; exp--; }
                    mant &= 0x03FFu;
                    f = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
                }
            } else if (exp == 0x1F) {
                f = sign | 0x7F800000u | (mant << 13);
            } else {
                f = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
            }
            float out;
            std::memcpy(&out, &f, sizeof(out));
            return out;
        }
    }
    return 0.0f;
}

/* 数值稳定 softmax，返回 (argmax_idx, max_prob)。 */
void SoftmaxArgmax(const TensorView& t, int n, int* arg, float* max_prob) {
    float m = ReadElem(t, 0);
    for (int i = 1; i < n; ++i) {
        float v = ReadElem(t, i);
        if (v > m) m = v;
    }
    float sum = 0.f;
    int   best = 0;
    float best_logit = ReadElem(t, 0);
    for (int i = 0; i < n; ++i) {
        float v = ReadElem(t, i);
        sum += std::exp(v - m);
        if (v > best_logit) { best_logit = v; best = i; }
    }
    *arg = best;
    *max_prob = std::exp(best_logit - m) / sum;
}

}  // namespace

Status DualheadClassifierPostprocessor::Configure(const IInferer& inferer,
                                                  const Json::Value& params) {
    head_a_index_     = params.get("head_a_index", 0).asInt();
    head_b_index_     = params.get("head_b_index", 1).asInt();
    head_a_classes_   = params.get("head_a_classes", 8).asInt();
    head_b_classes_   = params.get("head_b_classes", 2).asInt();
    is_3digit_class_  = params.get("is_3digit_class", 1).asInt();
    three_digit_idx_  = params.get("three_digit_class_index", head_a_classes_).asInt();
    conf_threshold_   = params.get("conf_threshold", 0.7f).asFloat();

    class_names_.clear();
    if (params.isMember("class_names") && params["class_names"].isArray()) {
        for (Json::ArrayIndex i = 0; i < params["class_names"].size(); ++i)
            class_names_.push_back(params["class_names"][i].asString());
    }
    category_ = params.get("category", "").asString();

    if (head_a_classes_ <= 0 || head_b_classes_ <= 0) {
        ALG_LOGE("dualhead_classifier: head sizes must be > 0 (a=%d b=%d)",
                 head_a_classes_, head_b_classes_);
        return ALG_E_POSTPROCESS;
    }
    if (inferer.NumOutputs() <= std::max(head_a_index_, head_b_index_)) {
        ALG_LOGE("dualhead_classifier: need outputs at idx %d & %d, but NumOutputs=%d",
                 head_a_index_, head_b_index_, inferer.NumOutputs());
        return ALG_E_POSTPROCESS;
    }

    /* 元素数校验：与 sample_speedsignnet.cpp 的运行期校验同义。 */
    const TensorView& a = inferer.OutputView(head_a_index_);
    const TensorView& b = inferer.OutputView(head_b_index_);
    if (a.shape.Numel() < head_a_classes_ || b.shape.Numel() < head_b_classes_) {
        ALG_LOGE("dualhead_classifier: head numel mismatch: a=%d (need %d), b=%d (need %d)",
                 a.shape.Numel(), head_a_classes_,
                 b.shape.Numel(), head_b_classes_);
        return ALG_E_POSTPROCESS;
    }

    configured_ = true;
    return ALG_OK;
}

Status DualheadClassifierPostprocessor::Apply(const IInferer& inferer,
                                              const PreprocessState& /*state*/,
                                              std::vector<Object>* out) {
    if (!configured_) return ALG_E_NOT_INITIALIZED;
    if (!out) return ALG_E_INVALID_ARG;
    out->clear();

    const TensorView& a = inferer.OutputView(head_a_index_);
    const TensorView& b = inferer.OutputView(head_b_index_);

    int arg_a, arg_b;
    float p_a, p_b;
    SoftmaxArgmax(a, head_a_classes_, &arg_a, &p_a);
    SoftmaxArgmax(b, head_b_classes_, &arg_b, &p_b);

    int   cls_id     = (arg_b == is_3digit_class_) ? three_digit_idx_ : arg_a;
    float joint_conf = p_a * p_b;

    /* 阈值过滤：低于阈值返回空 → ChainSolution `classify_into:` 触发 src 框 drop。 */
    if (joint_conf < conf_threshold_) return ALG_OK;

    Object o;
    o.field_mask = ALG_FIELD_BOX | ALG_FIELD_ATTRIBUTES;
    o.label     = cls_id;
    o.box.score = joint_conf;
    o.box.xmin = o.box.ymin = o.box.xmax = o.box.ymax = 0;  /* ROI 内坐标，ChainSolution 不会用 */

    Attribute attr;
    attr.name        = "class";
    attr.value_int   = cls_id;
    attr.value_float = joint_conf;
    if (cls_id >= 0 && cls_id < static_cast<int>(class_names_.size()))
        attr.value_str = class_names_[cls_id];
    o.attributes.push_back(std::move(attr));

    if (!category_.empty()) {
        Attribute c;
        c.name      = "category";
        c.value_str = category_;
        o.attributes.push_back(std::move(c));
    }

    out->push_back(std::move(o));
    return ALG_OK;
}

}  // namespace alg
