#include "models/face_attribute/face_attribute_postprocessor.h"

#include <algorithm>
#include <cmath>

#include "core/logger.h"

namespace alg {

namespace {

float ReadElem(const TensorView& t, int idx) {
    if (t.dtype == DataType::kF32) return static_cast<const float*>(t.data)[idx];
    if (t.dtype == DataType::kU8) {
        const uint8_t* p = static_cast<const uint8_t*>(t.data);
        return (p[idx] - t.quant.zero_point) * t.quant.scale;
    }
    return 0.0f;
}

FaceAttributePostprocessor::HeadKind ParseKind(const std::string& s) {
    if (s == "sigmoid") return FaceAttributePostprocessor::HeadKind::kSigmoid;
    if (s == "regress" || s == "regression")
        return FaceAttributePostprocessor::HeadKind::kRegress;
    return FaceAttributePostprocessor::HeadKind::kSoftmax;
}

int ArgMax(const TensorView& t) {
    const int n = t.shape.Numel();
    if (n <= 0) return 0;
    int best = 0;
    float best_v = ReadElem(t, 0);
    for (int i = 1; i < n; ++i) {
        float v = ReadElem(t, i);
        if (v > best_v) { best_v = v; best = i; }
    }
    return best;
}

}  // namespace

Status FaceAttributePostprocessor::Configure(const IInferer& inferer, const Json::Value& params) {
    heads_.clear();
    if (!params.isMember("labels") || !params["labels"].isArray()) {
        ALG_LOGE("face_attribute: 'labels' array required"); return ALG_E_POSTPROCESS;
    }
    const Json::Value& labels = params["labels"];
    for (Json::ArrayIndex i = 0; i < labels.size(); ++i) {
        const Json::Value& h = labels[i];
        Head head;
        head.name = h.get("name", "").asString();
        head.output_index = h.get("output_index", 0).asInt();
        head.kind = ParseKind(h.get("kind", "softmax").asString());
        head.threshold = h.get("threshold", 0.5f).asFloat();
        if (h.isMember("classes") && h["classes"].isArray()) {
            for (Json::ArrayIndex j = 0; j < h["classes"].size(); ++j)
                head.classes.push_back(h["classes"][j].asString());
        }
        if (head.output_index >= inferer.NumOutputs()) {
            ALG_LOGE("face_attribute head '%s': output_index %d out of range",
                     head.name.c_str(), head.output_index);
            return ALG_E_POSTPROCESS;
        }
        heads_.push_back(std::move(head));
    }
    configured_ = true;
    return ALG_OK;
}

Status FaceAttributePostprocessor::Apply(const IInferer& inferer, const PreprocessState& /*state*/,
                                         std::vector<Object>* out) {
    if (!configured_) return ALG_E_NOT_INITIALIZED;
    out->clear();

    Object o;
    o.field_mask = ALG_FIELD_ATTRIBUTES;
    o.attributes.reserve(heads_.size());

    for (const auto& h : heads_) {
        const TensorView& t = inferer.OutputView(h.output_index);
        Attribute a;
        a.name = h.name;
        switch (h.kind) {
            case HeadKind::kSoftmax: {
                int cls = ArgMax(t);
                a.value_int = cls;
                if (cls >= 0 && cls < static_cast<int>(h.classes.size()))
                    a.value_str = h.classes[cls];
                break;
            }
            case HeadKind::kSigmoid: {
                float raw = ReadElem(t, 0);
                float p = 1.0f / (1.0f + std::exp(-raw));
                a.value_float = p;
                a.value_int = (p >= h.threshold) ? 1 : 0;
                if (!h.classes.empty())
                    a.value_str = h.classes[std::min<int>(a.value_int, static_cast<int>(h.classes.size()) - 1)];
                break;
            }
            case HeadKind::kRegress: {
                a.value_float = ReadElem(t, 0);
                break;
            }
        }
        o.attributes.push_back(std::move(a));
    }
    out->push_back(std::move(o));
    return ALG_OK;
}

}  // namespace alg
