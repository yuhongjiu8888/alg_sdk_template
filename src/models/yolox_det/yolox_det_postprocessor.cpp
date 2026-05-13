#include "models/yolox_det/yolox_det_postprocessor.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/logger.h"
#include "core/postprocess/nms.h"

namespace alg {

namespace {

inline float Sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }

/* 反量化辅助：与 alg_traffic_light_detection/src/deploy_src/pre_post.cpp::dequant 等价。 */
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

struct LevelView {
    const TensorView* t;
    int   stride;
    int   H, W;
    int   channels;
    bool  is_nchw;
};

int BuildLevelView(const TensorView& t, int stride, int channels, LevelView* v) {
    v->t        = &t;
    v->stride   = stride;
    v->channels = channels;

    if (t.shape.ndims != 4 || t.shape.dims[0] != 1) {
        ALG_LOGE("yolox_det: %s ndims=%d dims[0]=%d, expect 4D batch=1",
                 t.name.c_str(), t.shape.ndims,
                 t.shape.ndims > 0 ? t.shape.dims[0] : 0);
        return -1;
    }
    /* 自动判定 NCHW vs NHWC：哪个非 batch 轴 == channels 即 channel 轴。 */
    if (t.shape.dims[1] == channels) {
        v->is_nchw = true;
        v->H = t.shape.dims[2];
        v->W = t.shape.dims[3];
    } else if (t.shape.dims[3] == channels) {
        v->is_nchw = false;
        v->H = t.shape.dims[1];
        v->W = t.shape.dims[2];
    } else {
        ALG_LOGE("yolox_det: %s no axis equals C=%d in (%d,%d,%d,%d)",
                 t.name.c_str(), channels,
                 t.shape.dims[0], t.shape.dims[1],
                 t.shape.dims[2], t.shape.dims[3]);
        return -1;
    }
    return 0;
}

inline int FlatIndex(const LevelView& v, int c, int y, int x) {
    if (v.is_nchw) return c * v.H * v.W + y * v.W + x;
    return (y * v.W + x) * v.channels + c;
}

void DecodeLevel(const LevelView& v, int num_classes, float obj_prefilter,
                 float score_thresh, std::vector<Proposal>* out) {
    const float stride = static_cast<float>(v.stride);
    for (int y = 0; y < v.H; ++y) {
        for (int x = 0; x < v.W; ++x) {
            float obj = Sigmoid(ReadElem(*v.t, FlatIndex(v, 4, y, x)));
            if (obj < obj_prefilter) continue;

            float best_cls = -1.f;
            int   best_idx = 0;
            for (int c = 0; c < num_classes; ++c) {
                float s = Sigmoid(ReadElem(*v.t, FlatIndex(v, 5 + c, y, x)));
                if (s > best_cls) { best_cls = s; best_idx = c; }
            }
            float score = obj * best_cls;
            if (score < score_thresh) continue;

            float cx_off = ReadElem(*v.t, FlatIndex(v, 0, y, x));
            float cy_off = ReadElem(*v.t, FlatIndex(v, 1, y, x));
            float w_log  = ReadElem(*v.t, FlatIndex(v, 2, y, x));
            float h_log  = ReadElem(*v.t, FlatIndex(v, 3, y, x));

            /* offset = 0：基准点是 grid 左上角。mmdet YOLOXHead 默认即此。 */
            float cx = cx_off * stride + static_cast<float>(x) * stride;
            float cy = cy_off * stride + static_cast<float>(y) * stride;
            float ww = std::exp(w_log) * stride;
            float hh = std::exp(h_log) * stride;

            Proposal p;
            p.x1 = cx - ww * 0.5f;
            p.y1 = cy - hh * 0.5f;
            p.x2 = cx + ww * 0.5f;
            p.y2 = cy + hh * 0.5f;
            p.score = score;
            p.label = best_idx;
            out->push_back(p);
        }
    }
}

}  // namespace

Status YoloxDetPostprocessor::Configure(const IInferer& inferer, const Json::Value& params) {
    num_classes_    = params.get("num_classes", 1).asInt();
    conf_threshold_ = params.get("conf_threshold", 0.4f).asFloat();
    nms_threshold_  = params.get("nms_threshold", 0.5f).asFloat();
    max_det_        = params.get("max_det", 100).asInt();
    obj_prefilter_  = params.get("obj_prefilter", 0.05f).asFloat();

    if (params.isMember("strides") && params["strides"].isArray()) {
        strides_.clear();
        for (Json::ArrayIndex i = 0; i < params["strides"].size(); ++i)
            strides_.push_back(params["strides"][i].asInt());
    }
    class_names_.clear();
    if (params.isMember("class_names") && params["class_names"].isArray()) {
        for (Json::ArrayIndex i = 0; i < params["class_names"].size(); ++i)
            class_names_.push_back(params["class_names"][i].asString());
    }
    category_ = params.get("category", "").asString();
    if (strides_.empty()) {
        ALG_LOGE("yolox_det: strides empty");
        return ALG_E_POSTPROCESS;
    }
    if (num_classes_ <= 0) {
        ALG_LOGE("yolox_det: num_classes must be > 0 (got %d)", num_classes_);
        return ALG_E_POSTPROCESS;
    }
    if (inferer.NumOutputs() < static_cast<int>(strides_.size())) {
        ALG_LOGE("yolox_det: need %zu outputs, got %d",
                 strides_.size(), inferer.NumOutputs());
        return ALG_E_POSTPROCESS;
    }
    configured_ = true;
    return ALG_OK;
}

Status YoloxDetPostprocessor::Apply(const IInferer& inferer, const PreprocessState& state,
                                    std::vector<Object>* out) {
    if (!configured_) return ALG_E_NOT_INITIALIZED;
    if (!out) return ALG_E_INVALID_ARG;
    out->clear();

    const int channels = 4 + 1 + num_classes_;
    props_.clear();
    if (props_.capacity() < 256) props_.reserve(256);

    /* 单尺度按 stride 解码到统一候选池；NMS 在最后一次性做（class-aware）。 */
    for (size_t i = 0; i < strides_.size(); ++i) {
        LevelView v;
        if (BuildLevelView(inferer.OutputView(static_cast<int>(i)),
                           strides_[i], channels, &v) != 0)
            return ALG_E_POSTPROCESS;
        DecodeLevel(v, num_classes_, obj_prefilter_, conf_threshold_, &props_);
    }

    Nms(props_, nms_threshold_, &nms_scratch_);
    if (static_cast<int>(props_.size()) > max_det_) props_.resize(max_det_);

    /* letterbox 反映射回原图。 */
    const float inv_s = state.scale_ratio > 0 ? 1.0f / state.scale_ratio : 1.0f;
    const int max_x = state.original_width  - 1;
    const int max_y = state.original_height - 1;

    const bool emit_class_attr = !class_names_.empty();
    const bool emit_category_attr = !category_.empty();

    out->reserve(props_.size());
    for (const auto& p : props_) {
        Object o;
        o.field_mask = ALG_FIELD_BOX;
        o.box.score = p.score;
        o.box.label = p.label;
        int x1 = static_cast<int>((p.x1 - state.pad_left) * inv_s + 0.5f);
        int y1 = static_cast<int>((p.y1 - state.pad_top)  * inv_s + 0.5f);
        int x2 = static_cast<int>((p.x2 - state.pad_left) * inv_s + 0.5f);
        int y2 = static_cast<int>((p.y2 - state.pad_top)  * inv_s + 0.5f);
        o.box.xmin = x1 < 0 ? 0 : (x1 > max_x ? max_x : x1);
        o.box.ymin = y1 < 0 ? 0 : (y1 > max_y ? max_y : y1);
        o.box.xmax = x2 < 0 ? 0 : (x2 > max_x ? max_x : x2);
        o.box.ymax = y2 < 0 ? 0 : (y2 > max_y ? max_y : y2);

        if (emit_class_attr || emit_category_attr) {
            o.field_mask |= ALG_FIELD_ATTRIBUTES;
            if (emit_class_attr) {
                Attribute a;
                a.name = "class";
                a.value_int = p.label;
                a.value_float = p.score;
                if (p.label >= 0 && p.label < static_cast<int>(class_names_.size()))
                    a.value_str = class_names_[p.label];
                o.attributes.push_back(std::move(a));
            }
            if (emit_category_attr) {
                Attribute a;
                a.name = "category";
                a.value_str = category_;
                o.attributes.push_back(std::move(a));
            }
        }

        out->push_back(std::move(o));
    }
    return ALG_OK;
}

}  // namespace alg
