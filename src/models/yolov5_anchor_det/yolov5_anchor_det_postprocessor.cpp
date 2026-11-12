#include "models/yolov5_anchor_det/yolov5_anchor_det_postprocessor.h"
#include <algorithm>
#include <cmath>
#include "core/logger.h"
#include "core/postprocess/box_transform.h"
#include "core/postprocess/math_utils.h"
#include "core/postprocess/nms.h"

namespace alg {

Status Yolov5AnchorDetPostprocessor::Configure(const IInferer& inferer, const Json::Value& params) {
    num_classes_ = params.get("num_classes", 1).asInt();
    bbox_channels_ = params.get("bbox_channels", 4).asInt();
    obj_channels_ = params.get("obj_channels", 1).asInt();
    stride_ = params.get("stride", 8).asInt();
    conf_threshold_ = params.get("conf_threshold", 0.25f).asFloat();
    nms_threshold_ = params.get("nms_threshold", 0.45f).asFloat();
    max_det_ = params.get("max_det", 5).asInt();
    obj_prefilter_ = params.get("obj_prefilter", 0.05f).asFloat();
    obj_logit_threshold_ = LogitThreshold(std::max(obj_prefilter_, conf_threshold_));

    if (params.isMember("anchor") && params["anchor"].isArray() && params["anchor"].size() == 2) {
        anchor_w_ = params["anchor"][0].asFloat();
        anchor_h_ = params["anchor"][1].asFloat();
    }
    if (num_classes_ <= 0 || stride_ <= 0) {
        ALG_LOGE("yolov5_anchor_det: num_classes(%d) and stride(%d) must be > 0", num_classes_, stride_);
        return ALG_E_POSTPROCESS;
    }
    if (bbox_channels_ <= 0) {
        ALG_LOGE("yolov5_anchor_det: bbox_channels must be > 0 (got %d)", bbox_channels_);
        return ALG_E_POSTPROCESS;
    }
    if (obj_channels_ < 0) {
        ALG_LOGE("yolov5_anchor_det: obj_channels must be >= 0 (got %d)", obj_channels_);
        return ALG_E_POSTPROCESS;
    }
    cls_offset_ = bbox_channels_ + obj_channels_;
    if (inferer.NumOutputs() < 1) {
        ALG_LOGE("yolov5_anchor_det: need at least 1 output");
        return ALG_E_POSTPROCESS;
    }
    configured_ = true;
    return ALG_OK;
}

Status Yolov5AnchorDetPostprocessor::Apply(const IInferer& inferer,
                                           const PreprocessState& state,
                                           std::vector<Object>* out) {
    if (!configured_)
        return ALG_E_NOT_INITIALIZED;
    if (!out)
        return ALG_E_INVALID_ARG;
    out->clear();

    const TensorView& t = inferer.OutputView(0);
    const int channels = cls_offset_ + num_classes_;

    if (t.shape.ndims != 4 || t.shape.dims[0] != 1) {
        ALG_LOGE("yolov5_anchor_det: %s ndims=%d dims[0]=%d, expect 4D batch=1", t.name.c_str(), t.shape.ndims,
                 t.shape.ndims > 0 ? t.shape.dims[0] : 0);
        return ALG_E_POSTPROCESS;
    }
    bool is_nchw;
    int H, W;
    if (t.shape.dims[1] == channels) {
        is_nchw = true;
        H = t.shape.dims[2];
        W = t.shape.dims[3];
    } else if (t.shape.dims[3] == channels) {
        is_nchw = false;
        H = t.shape.dims[1];
        W = t.shape.dims[2];
    } else {
        ALG_LOGE("yolov5_anchor_det: %s no axis equals C=%d in (%d,%d,%d,%d)", t.name.c_str(), channels,
                 t.shape.dims[0], t.shape.dims[1], t.shape.dims[2], t.shape.dims[3]);
        return ALG_E_POSTPROCESS;
    }

    auto idx_at = [&](int c, int y, int x) -> int {
        if (is_nchw)
            return c * H * W + y * W + x;
        return (y * W + x) * channels + c;
    };

    props_.clear();
    if (props_.capacity() < 256)
        props_.reserve(256);

    const float fs = static_cast<float>(stride_);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const float obj_logit = ReadElem(t, idx_at(bbox_channels_, y, x));
            if (obj_logit < obj_logit_threshold_)
                continue;
            float obj = Sigmoid(obj_logit);
            if (obj < obj_prefilter_)
                continue;

            /* cls 分支用 softmax（与训练 model_src/postprocess.py 严格一致）。
             * 1 类时 softmax 恒为 1 → score=obj；若用 sigmoid，会乘上一个未受监督
             * （softmax-1类 CE 梯度为 0）的任意 cls logit，把分数压到 conf_threshold
             * 以下造成漏检——这正是板端比 ONNX 少检的根因。 */
            int best_idx = 0;
            float max_logit = ReadElem(t, idx_at(cls_offset_, y, x));
            for (int c = 1; c < num_classes_; ++c) {
                float l = ReadElem(t, idx_at(cls_offset_ + c, y, x));
                if (l > max_logit) {
                    max_logit = l;
                    best_idx = c;
                }
            }
            float sum_exp = 0.f;
            for (int c = 0; c < num_classes_; ++c)
                sum_exp += std::exp(ReadElem(t, idx_at(cls_offset_ + c, y, x)) - max_logit);
            float best_cls = 1.0f / sum_exp; /* = softmax 最大概率 exp(0)/Σexp */
            float score = obj * best_cls;
            if (score < conf_threshold_)
                continue;

            float tx = ReadElem(t, idx_at(0, y, x));
            float ty = ReadElem(t, idx_at(1, y, x));
            float tw = ReadElem(t, idx_at(2, y, x));
            float th = ReadElem(t, idx_at(3, y, x));

            float bx = (Sigmoid(tx) * 2.0f - 0.5f + static_cast<float>(x)) * fs;
            float by = (Sigmoid(ty) * 2.0f - 0.5f + static_cast<float>(y)) * fs;
            float ww = Sigmoid(tw) * 2.0f;
            ww = ww * ww * anchor_w_;
            float hh = Sigmoid(th) * 2.0f;
            hh = hh * hh * anchor_h_;

            Proposal p;
            p.x1 = bx - ww * 0.5f;
            p.y1 = by - hh * 0.5f;
            p.x2 = bx + ww * 0.5f;
            p.y2 = by + hh * 0.5f;
            p.score = score;
            p.label = best_idx;
            props_.push_back(p);
        }
    }

    Nms(props_, nms_threshold_, &nms_scratch_, max_det_);

    const DetectionBoxTransform box_transform(state);

    out->reserve(props_.size());
    for (const auto& p : props_) {
        Object o;
        o.field_mask = ALG_FIELD_BOX;
        o.box = box_transform.Apply(p);
        o.label = p.label;

        out->push_back(std::move(o));
    }
    return ALG_OK;
}

}  // namespace alg
