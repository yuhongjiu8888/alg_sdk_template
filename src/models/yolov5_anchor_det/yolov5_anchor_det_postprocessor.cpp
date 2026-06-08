#include "models/yolov5_anchor_det/yolov5_anchor_det_postprocessor.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/logger.h"
#include "core/postprocess/nms.h"

namespace alg {

namespace {

inline float Sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }

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

}  // namespace

Status Yolov5AnchorDetPostprocessor::Configure(const IInferer& inferer, const Json::Value& params) {
    num_classes_    = params.get("num_classes", 1).asInt();
    bbox_channels_  = params.get("bbox_channels", 4).asInt();
    obj_channels_   = params.get("obj_channels", 1).asInt();
    stride_         = params.get("stride", 8).asInt();
    conf_threshold_ = params.get("conf_threshold", 0.25f).asFloat();
    nms_threshold_  = params.get("nms_threshold", 0.45f).asFloat();
    max_det_        = params.get("max_det", 64).asInt();
    obj_prefilter_  = params.get("obj_prefilter", 0.05f).asFloat();

    if (params.isMember("anchor") && params["anchor"].isArray() &&
        params["anchor"].size() == 2) {
        anchor_w_ = params["anchor"][0].asFloat();
        anchor_h_ = params["anchor"][1].asFloat();
    }
    if (num_classes_ <= 0 || stride_ <= 0) {
        ALG_LOGE("yolov5_anchor_det: num_classes(%d) and stride(%d) must be > 0",
                 num_classes_, stride_);
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

Status Yolov5AnchorDetPostprocessor::Apply(const IInferer& inferer, const PreprocessState& state,
                                           std::vector<Object>* out) {
    if (!configured_) return ALG_E_NOT_INITIALIZED;
    if (!out) return ALG_E_INVALID_ARG;
    out->clear();

    const TensorView& t = inferer.OutputView(0);
    const int channels = cls_offset_ + num_classes_;

    if (t.shape.ndims != 4 || t.shape.dims[0] != 1) {
        ALG_LOGE("yolov5_anchor_det: %s ndims=%d dims[0]=%d, expect 4D batch=1",
                 t.name.c_str(), t.shape.ndims,
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
        ALG_LOGE("yolov5_anchor_det: %s no axis equals C=%d in (%d,%d,%d,%d)",
                 t.name.c_str(), channels,
                 t.shape.dims[0], t.shape.dims[1],
                 t.shape.dims[2], t.shape.dims[3]);
        return ALG_E_POSTPROCESS;
    }

    auto idx_at = [&](int c, int y, int x) -> int {
        if (is_nchw) return c * H * W + y * W + x;
        return (y * W + x) * channels + c;
    };

    props_.clear();
    if (props_.capacity() < 256) props_.reserve(256);

    /* —— 一次性诊断：板端 0 检出时，先确认检测头到底有没有响应 ——
     * 全网格扫一遍（不卡阈值），看 max obj / max score 落在哪、有多少格子
     * 过 prefilter / conf；再看输出原始字节 min/max（判断是不是常数死值）。
     * 确认后删除。 */
    {
        static bool dumped = false;
        if (!dumped) {
            dumped = true;
            float max_obj = -1.f, max_score = -1.f;
            int   mx = -1, my = -1, n_pre = 0, n_conf = 0;
            int   raw_min = 255, raw_max = 0;
            const int total = channels * H * W;
            for (int i = 0; i < total; ++i) {
                int rv = (t.dtype == DataType::kU8)
                             ? static_cast<const uint8_t*>(t.data)[i]
                             : (t.dtype == DataType::kI8
                                    ? static_cast<const int8_t*>(t.data)[i] + 128
                                    : 0);
                if (rv < raw_min) raw_min = rv;
                if (rv > raw_max) raw_max = rv;
            }
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    float o = Sigmoid(ReadElem(t, idx_at(bbox_channels_, y, x)));
                    float ml = ReadElem(t, idx_at(cls_offset_, y, x));
                    for (int c = 1; c < num_classes_; ++c) {
                        float l = ReadElem(t, idx_at(cls_offset_ + c, y, x));
                        if (l > ml) ml = l;
                    }
                    float se = 0.f;
                    for (int c = 0; c < num_classes_; ++c)
                        se += std::exp(ReadElem(t, idx_at(cls_offset_ + c, y, x)) - ml);
                    float sc = o * (1.0f / se);
                    if (o >= obj_prefilter_) ++n_pre;
                    if (sc >= conf_threshold_) ++n_conf;
                    if (o > max_obj) { max_obj = o; mx = x; my = y; }
                    if (sc > max_score) max_score = sc;
                }
            ALG_LOGW("[det-diag] raw[min=%d max=%d] scale=%g zp=%d | max_obj=%.4f @(%d,%d) "
                     "max_score=%.4f | n_pre(>=%.2f)=%d n_conf(>=%.2f)=%d / %d cells",
                     raw_min, raw_max, t.quant.scale, t.quant.zero_point, max_obj, mx, my,
                     max_score, obj_prefilter_, n_pre, conf_threshold_, n_conf, H * W);
        }
    }

    const float fs = static_cast<float>(stride_);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float obj = Sigmoid(ReadElem(t, idx_at(bbox_channels_, y, x)));
            if (obj < obj_prefilter_) continue;

            /* cls 分支用 softmax（与训练 model_src/postprocess.py 严格一致）。
             * 1 类时 softmax 恒为 1 → score=obj；若用 sigmoid，会乘上一个未受监督
             * （softmax-1类 CE 梯度为 0）的任意 cls logit，把分数压到 conf_threshold
             * 以下造成漏检——这正是板端比 ONNX 少检的根因。 */
            int   best_idx   = 0;
            float max_logit  = ReadElem(t, idx_at(cls_offset_, y, x));
            for (int c = 1; c < num_classes_; ++c) {
                float l = ReadElem(t, idx_at(cls_offset_ + c, y, x));
                if (l > max_logit) { max_logit = l; best_idx = c; }
            }
            float sum_exp = 0.f;
            for (int c = 0; c < num_classes_; ++c)
                sum_exp += std::exp(ReadElem(t, idx_at(cls_offset_ + c, y, x)) - max_logit);
            float best_cls = 1.0f / sum_exp;   /* = softmax 最大概率 exp(0)/Σexp */
            float score = obj * best_cls;
            if (score < conf_threshold_) continue;

            float tx = ReadElem(t, idx_at(0, y, x));
            float ty = ReadElem(t, idx_at(1, y, x));
            float tw = ReadElem(t, idx_at(2, y, x));
            float th = ReadElem(t, idx_at(3, y, x));

            float bx = (Sigmoid(tx) * 2.0f - 0.5f + static_cast<float>(x)) * fs;
            float by = (Sigmoid(ty) * 2.0f - 0.5f + static_cast<float>(y)) * fs;
            float ww = Sigmoid(tw) * 2.0f; ww = ww * ww * anchor_w_;
            float hh = Sigmoid(th) * 2.0f; hh = hh * hh * anchor_h_;

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

    Nms(props_, nms_threshold_, &nms_scratch_);
    if (static_cast<int>(props_.size()) > max_det_) props_.resize(max_det_);

    {
        static bool dumped2 = false;
        if (!dumped2) {
            dumped2 = true;
            ALG_LOGW("[det-diag] NMS 后最终框数=%zu（这些会进 stage2 分类）", props_.size());
        }
    }

    const float inv_s = state.scale_ratio > 0 ? 1.0f / state.scale_ratio : 1.0f;
    const int max_x = state.original_width  - 1;
    const int max_y = state.original_height - 1;

    out->reserve(props_.size());
    for (const auto& p : props_) {
        Object o;
        o.field_mask = ALG_FIELD_BOX;
        o.box.score = p.score;
        o.label     = p.label;
        int x1 = static_cast<int>((p.x1 - state.pad_left) * inv_s + 0.5f);
        int y1 = static_cast<int>((p.y1 - state.pad_top)  * inv_s + 0.5f);
        int x2 = static_cast<int>((p.x2 - state.pad_left) * inv_s + 0.5f);
        int y2 = static_cast<int>((p.y2 - state.pad_top)  * inv_s + 0.5f);
        o.box.xmin = x1 < 0 ? 0 : (x1 > max_x ? max_x : x1);
        o.box.ymin = y1 < 0 ? 0 : (y1 > max_y ? max_y : y1);
        o.box.xmax = x2 < 0 ? 0 : (x2 > max_x ? max_x : x2);
        o.box.ymax = y2 < 0 ? 0 : (y2 > max_y ? max_y : y2);
        out->push_back(std::move(o));
    }
    return ALG_OK;
}

}  // namespace alg
