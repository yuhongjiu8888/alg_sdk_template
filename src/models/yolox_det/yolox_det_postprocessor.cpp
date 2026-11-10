#include "models/yolox_det/yolox_det_postprocessor.h"
#include <algorithm>
#include <cmath>
#include "core/logger.h"
#include "core/postprocess/box_transform.h"
#include "core/postprocess/math_utils.h"
#include "core/postprocess/nms.h"

namespace alg {

namespace {

struct LevelView {
    const TensorView* t;
    int stride;
    int H, W;
    int channels;
    bool is_nchw;
};

int BuildLevelView(const TensorView& t, int stride, int channels, LevelView* v) {
    v->t = &t;
    v->stride = stride;
    v->channels = channels;

    if (t.shape.ndims != 4 || t.shape.dims[0] != 1) {
        ALG_LOGE("yolox_det: %s ndims=%d dims[0]=%d, expect 4D batch=1", t.name.c_str(), t.shape.ndims,
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
        ALG_LOGE("yolox_det: %s no axis equals C=%d in (%d,%d,%d,%d)", t.name.c_str(), channels, t.shape.dims[0],
                 t.shape.dims[1], t.shape.dims[2], t.shape.dims[3]);
        return -1;
    }
    return 0;
}

inline int FlatIndex(const LevelView& v, int c, int y, int x) {
    if (v.is_nchw)
        return c * v.H * v.W + y * v.W + x;
    return (y * v.W + x) * v.channels + c;
}

void DecodeLevel(const LevelView& v,
                 int num_classes,
                 int obj_offset,
                 int cls_offset,
                 float obj_prefilter,
                 float obj_logit_threshold,
                 float score_thresh,
                 std::vector<Proposal>* out) {
    const float stride = static_cast<float>(v.stride);
    for (int y = 0; y < v.H; ++y) {
        for (int x = 0; x < v.W; ++x) {
            const float obj_logit = ReadElem(*v.t, FlatIndex(v, obj_offset, y, x));
            if (obj_logit < obj_logit_threshold)
                continue;
            float obj = Sigmoid(obj_logit);
            if (obj < obj_prefilter)
                continue;

            float best_cls = -1.f;
            int best_idx = 0;
            for (int c = 0; c < num_classes; ++c) {
                float s = Sigmoid(ReadElem(*v.t, FlatIndex(v, cls_offset + c, y, x)));
                if (s > best_cls) {
                    best_cls = s;
                    best_idx = c;
                }
            }
            float score = obj * best_cls;
            if (score < score_thresh)
                continue;

            float cx_off = ReadElem(*v.t, FlatIndex(v, 0, y, x));
            float cy_off = ReadElem(*v.t, FlatIndex(v, 1, y, x));
            float w_log = ReadElem(*v.t, FlatIndex(v, 2, y, x));
            float h_log = ReadElem(*v.t, FlatIndex(v, 3, y, x));

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
    num_classes_ = params.get("num_classes", 1).asInt();
    bbox_channels_ = params.get("bbox_channels", 4).asInt();
    obj_channels_ = params.get("obj_channels", 1).asInt();
    conf_threshold_ = params.get("conf_threshold", 0.4f).asFloat();
    nms_threshold_ = params.get("nms_threshold", 0.5f).asFloat();
    max_det_ = params.get("max_det", 100).asInt();
    obj_prefilter_ = params.get("obj_prefilter", 0.05f).asFloat();
    obj_logit_threshold_ = LogitThreshold(std::max(obj_prefilter_, conf_threshold_));

    if (params.isMember("strides") && params["strides"].isArray()) {
        strides_.clear();
        for (Json::ArrayIndex i = 0; i < params["strides"].size(); ++i)
            strides_.push_back(params["strides"][i].asInt());
    }
    class_names_.clear();
    class_values_.clear();
    if (params.isMember("class_names") && params["class_names"].isArray()) {
        for (Json::ArrayIndex i = 0; i < params["class_names"].size(); ++i)
            class_names_.push_back(params["class_names"][i].asString());
    }
    category_ = params.get("category", "").asString();
    /* traffic_light 类别：从 class_names 推导颜色 enum 值 */
    if (category_ == "traffic_light") {
        for (const auto& name : class_names_) {
            if (name == "red" || name == "red_light")
                class_values_.push_back(1);
            else if (name == "yellow" || name == "yellow_light")
                class_values_.push_back(2);
            else if (name == "green" || name == "green_light")
                class_values_.push_back(3);
            else if (name == "off" || name == "off_light")
                class_values_.push_back(4);
            else
                class_values_.push_back(0);
        }
    }
    if (strides_.empty()) {
        ALG_LOGE("yolox_det: strides empty");
        return ALG_E_POSTPROCESS;
    }
    if (num_classes_ <= 0) {
        ALG_LOGE("yolox_det: num_classes must be > 0 (got %d)", num_classes_);
        return ALG_E_POSTPROCESS;
    }
    if (bbox_channels_ <= 0) {
        ALG_LOGE("yolox_det: bbox_channels must be > 0 (got %d)", bbox_channels_);
        return ALG_E_POSTPROCESS;
    }
    if (obj_channels_ < 0) {
        ALG_LOGE("yolox_det: obj_channels must be >= 0 (got %d)", obj_channels_);
        return ALG_E_POSTPROCESS;
    }
    cls_offset_ = bbox_channels_ + obj_channels_;
    if (inferer.NumOutputs() < static_cast<int>(strides_.size())) {
        ALG_LOGE("yolox_det: need %zu outputs, got %d", strides_.size(), inferer.NumOutputs());
        return ALG_E_POSTPROCESS;
    }
    configured_ = true;
    return ALG_OK;
}

Status YoloxDetPostprocessor::Apply(const IInferer& inferer, const PreprocessState& state, std::vector<Object>* out) {
    if (!configured_)
        return ALG_E_NOT_INITIALIZED;
    if (!out)
        return ALG_E_INVALID_ARG;
    out->clear();

    const int channels = cls_offset_ + num_classes_;
    props_.clear();
    if (props_.capacity() < 256)
        props_.reserve(256);

    /* 单尺度按 stride 解码到统一候选池；NMS 在最后一次性做（class-aware）。 */
    for (size_t i = 0; i < strides_.size(); ++i) {
        LevelView v;
        if (BuildLevelView(inferer.OutputView(static_cast<int>(i)), strides_[i], channels, &v) != 0)
            return ALG_E_POSTPROCESS;
        DecodeLevel(v, num_classes_, bbox_channels_, cls_offset_, obj_prefilter_, obj_logit_threshold_, conf_threshold_,
                    &props_);
    }

    Nms(props_, nms_threshold_, &nms_scratch_, max_det_);

    /* letterbox 反映射回原图。 */
    const DetectionBoxTransform box_transform(state);

    const bool emit_class_attr = !class_names_.empty();
    const bool emit_category_attr = !category_.empty();

    out->reserve(props_.size());
    for (const auto& p : props_) {
        Object o;
        o.field_mask = ALG_FIELD_BOX;
        o.box = box_transform.Apply(p);
        o.label = p.label;
        if (p.label >= 0 && p.label < static_cast<int>(class_values_.size()))
            o.value = class_values_[p.label];

        if (emit_class_attr || emit_category_attr) {
            o.field_mask |= ALG_FIELD_ATTRIBUTES;
            if (emit_class_attr) {
                Attribute a;
                a.name = "class";
                a.value_int = o.label;
                a.value_float = p.score;
                if (o.label >= 0 && o.label < static_cast<int>(class_names_.size()))
                    a.value_str = class_names_[o.label];
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
