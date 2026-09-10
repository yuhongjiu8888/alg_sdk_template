#include "models/rtmdet_det/rtmdet_det_postprocessor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "core/logger.h"
#include "core/postprocess/nms.h"

namespace alg {

namespace {

inline float Sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }

inline float Logit(float p) {
    if (p <= 0.0f) return -std::numeric_limits<float>::infinity();
    if (p >= 1.0f) return std::numeric_limits<float>::infinity();
    /* 略向低侧放宽，避免 log/sigmoid 浮点舍入在阈值边界误剪。 */
    return std::log(p / (1.0f - p)) - 1e-6f;
}

inline float Clamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* 反量化辅助：与其它后处理器一致，支持 FLOAT / FLOAT16 / 量化整型。 */
inline float ReadElem(const TensorView& t, size_t idx) {
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

/* 按 tensor 实际布局取 (c, y, x) 处的扁平元素下标。
 * NCHW：支持 ATC 按行对齐的输出（row_stride>0 时每行实际元素数 > W，按
 * row_stride 定位；dense 时为 W）。NHWC 按紧凑行主序（目前无非紧凑先例）。 */
inline size_t FlatIndex(const TensorView& t, int c, int y, int x) {
    if (t.layout == Layout::kNHWC) {
        const int W = t.shape.dims[2];
        const int C = t.shape.dims[3];
        return static_cast<size_t>(y * W + x) * C + c;
    }
    const int H = t.shape.dims[2];
    const int W = t.shape.dims[3];
    const size_t elem = static_cast<size_t>(BytesPerElement(t.dtype));
    const size_t row_elem = t.row_stride ? (t.row_stride / elem) : W;
    return (static_cast<size_t>(c) * H + y) * row_elem + x;
}

}  // namespace

Status RtmdetDetPostprocessor::Configure(const IInferer& inferer, const Json::Value& params) {
    num_classes_    = params.get("num_classes", 1).asInt();
    bbox_channels_  = params.get("bbox_channels", 4).asInt();
    conf_threshold_ = params.get("conf_threshold", 0.25f).asFloat();
    nms_threshold_  = params.get("nms_threshold", 0.45f).asFloat();
    min_bbox_size_  = params.get("min_bbox_size", 4.0f).asFloat();
    max_det_        = params.get("max_det", 2).asInt();
    conf_logit_threshold_ = Logit(conf_threshold_);
    category_       = params.get("category", "").asString();

    strides_.clear();
    if (params.isMember("strides") && params["strides"].isArray()) {
        for (Json::ArrayIndex i = 0; i < params["strides"].size(); ++i)
            strides_.push_back(params["strides"][i].asInt());
    }
    if (strides_.empty()) {
        strides_.assign({8, 16, 32});
    }

    /* 模型输入尺寸（stride 由 input_h / feat_h 推导）：从 postprocess 参数读
     * input_size=[W,H]（与 preprocess 一致，框架不把输入视图暴露给后处理）。 */
    input_w_ = input_h_ = 0;
    if (params.isMember("input_size") && params["input_size"].isArray() &&
        params["input_size"].size() == 2) {
        input_w_ = params["input_size"][0].asInt();
        input_h_ = params["input_size"][1].asInt();
    }

    if (num_classes_ <= 0 || bbox_channels_ <= 0) {
        ALG_LOGE("rtmdet_det: num_classes(%d) / bbox_channels(%d) must be > 0",
                 num_classes_, bbox_channels_);
        return ALG_E_POSTPROCESS;
    }
    if (inferer.NumOutputs() < 1) {
        ALG_LOGE("rtmdet_det: need at least 1 output");
        return ALG_E_POSTPROCESS;
    }

    /* 先按「通道数 == num_classes → cls、== bbox_channels → reg」收集候选 head。 */
    struct HeadCandidate { int index, h, w; };
    std::vector<HeadCandidate> cls_cand, reg_cand;
    for (int i = 0; i < inferer.NumOutputs(); ++i) {
        const TensorView& t = inferer.OutputView(i);
        if (t.shape.ndims != 4 || t.shape.dims[0] != 1) continue;
        const int channels = (t.layout == Layout::kNHWC) ? t.shape.dims[3] : t.shape.dims[1];
        const int h = t.shape.dims[2];
        const int w = t.shape.dims[3];
        if (channels == num_classes_) cls_cand.push_back({i, h, w});
        else if (channels == bbox_channels_) reg_cand.push_back({i, h, w});
    }
    if (cls_cand.size() < strides_.size() || reg_cand.size() < strides_.size()) {
        ALG_LOGE("rtmdet_det: head 数量不足 cls=%zu reg=%zu (need %zu)",
                 cls_cand.size(), reg_cand.size(), strides_.size());
        return ALG_E_POSTPROCESS;
    }

    /* 各候选的 stride：给定 input_size 时按 input_h / feat_h 精确匹配；
     * 缺省时退化为按特征图面积从大到小对应 strides（RTMDet 最小 stride=8 的特征图最大）。 */
    cls_idx_.assign(strides_.size(), -1);
    reg_idx_.assign(strides_.size(), -1);
    if (input_h_ > 0 && input_w_ > 0) {
        for (const HeadCandidate& c : cls_cand) {
            if (input_h_ % c.h != 0 || input_w_ % c.w != 0) continue;
            const int stride = input_h_ / c.h;
            for (size_t l = 0; l < strides_.size(); ++l) {
                if (strides_[l] == stride && cls_idx_[l] < 0) {
                    cls_idx_[l] = c.index;
                    ALG_LOGI("rtmdet_det: cls output[%d] '%s' %dx%d stride=%d -> level %zu",
                             c.index, inferer.OutputView(c.index).name.c_str(), c.h, c.w,
                             stride, l);
                    break;
                }
            }
        }
        for (const HeadCandidate& c : reg_cand) {
            if (input_h_ % c.h != 0 || input_w_ % c.w != 0) continue;
            const int stride = input_h_ / c.h;
            for (size_t l = 0; l < strides_.size(); ++l) {
                if (strides_[l] == stride && reg_idx_[l] < 0) {
                    reg_idx_[l] = c.index;
                    ALG_LOGI("rtmdet_det: reg output[%d] '%s' %dx%d stride=%d -> level %zu",
                             c.index, inferer.OutputView(c.index).name.c_str(), c.h, c.w,
                             stride, l);
                    break;
                }
            }
        }
    } else {
        /* 无 input_size：cls 候选按面积降序、reg 候选按面积降序，与 strides 升序一一对应。 */
        std::sort(cls_cand.begin(), cls_cand.end(),
                  [](const HeadCandidate& a, const HeadCandidate& b) {
                      return a.h * a.w > b.h * b.w;
                  });
        std::sort(reg_cand.begin(), reg_cand.end(),
                  [](const HeadCandidate& a, const HeadCandidate& b) {
                      return a.h * a.w > b.h * b.w;
                  });
        for (size_t l = 0; l < strides_.size(); ++l) {
            cls_idx_[l] = cls_cand[l].index;
            reg_idx_[l] = reg_cand[l].index;
            ALG_LOGI("rtmdet_det: fallback level %d -> cls[%d] '%s' reg[%d] '%s' (stride=%d)",
                     static_cast<int>(l), cls_cand[l].index,
                     inferer.OutputView(cls_cand[l].index).name.c_str(),
                     reg_cand[l].index,
                     inferer.OutputView(reg_cand[l].index).name.c_str(), strides_[l]);
        }
    }
    for (size_t l = 0; l < strides_.size(); ++l) {
        if (cls_idx_[l] < 0 || reg_idx_[l] < 0) {
            ALG_LOGE("rtmdet_det: stride=%d head 未找齐 (cls=%d reg=%d)",
                     strides_[l], cls_idx_[l], reg_idx_[l]);
            return ALG_E_POSTPROCESS;
        }
    }

    ALG_LOGI("rtmdet_det: ready, input=%dx%d classes=%d strides=%zu thr=%.2f nms=%.2f max_det=%d",
             input_w_, input_h_, num_classes_, strides_.size(), conf_threshold_,
             nms_threshold_, max_det_);
    configured_ = true;
    return ALG_OK;
}

Status RtmdetDetPostprocessor::Apply(const IInferer& inferer, const PreprocessState& state,
                                     std::vector<Object>* out) {
    if (!configured_) return ALG_E_NOT_INITIALIZED;
    if (!out) return ALG_E_INVALID_ARG;
    out->clear();

    const int input_w = input_w_;
    const int input_h = input_h_;

    /* 1. 逐 stride 解码：prior=(grid+0)*stride，框=[px-l,py-t,px+r,py+b]。 */
    props_.clear();
    if (props_.capacity() < 128) props_.reserve(128);
    for (size_t level = 0; level < strides_.size(); ++level) {
        const TensorView& cls = inferer.OutputView(cls_idx_[level]);
        const TensorView& reg = inferer.OutputView(reg_idx_[level]);
        const int H = input_h / strides_[level];
        const int W = input_w / strides_[level];
        const int stride = strides_[level];
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const size_t p = FlatIndex(cls, 0, y, x);
                const float score_logit = ReadElem(cls, p);
                if (score_logit < conf_logit_threshold_) continue;
                const float score = Sigmoid(score_logit);
                if (score < conf_threshold_) continue;
                const float left   = ReadElem(reg, FlatIndex(reg, 0, y, x));
                const float top    = ReadElem(reg, FlatIndex(reg, 1, y, x));
                const float right  = ReadElem(reg, FlatIndex(reg, 2, y, x));
                const float bottom = ReadElem(reg, FlatIndex(reg, 3, y, x));
                const float prior_x = static_cast<float>(x) * stride;
                const float prior_y = static_cast<float>(y) * stride;
                Proposal p_;
                p_.x1 = prior_x - left;
                p_.y1 = prior_y - top;
                p_.x2 = prior_x + right;
                p_.y2 = prior_y + bottom;
                p_.score = score;
                p_.label = 0;
                props_.push_back(p_);
            }
        }
    }

    /* 2. clip 到模型输入空间，过滤过小框（demo 的 min_bbox_size=4）。 */
    filtered_.clear();
    filtered_.reserve(props_.size());
    for (const Proposal& c : props_) {
        const float x1 = Clamp(c.x1, 0.0f, static_cast<float>(input_w));
        const float y1 = Clamp(c.y1, 0.0f, static_cast<float>(input_h));
        const float x2 = Clamp(c.x2, 0.0f, static_cast<float>(input_w));
        const float y2 = Clamp(c.y2, 0.0f, static_cast<float>(input_h));
        if (x2 - x1 <= min_bbox_size_ || y2 - y1 <= min_bbox_size_) continue;
        Proposal p;
        p.x1 = x1; p.y1 = y1; p.x2 = x2; p.y2 = y2;
        p.score = c.score;
        p.label = 0;
        filtered_.push_back(p);
    }

    /* 3. 单类 NMS + 截断 max_det。 */
    Nms(filtered_, nms_threshold_, &nms_scratch_, max_det_);

    /* —— 一次性诊断：板端 0 检出时确认检测头是否有响应（原始 min/max、最强框）。 */
    {
        static bool dumped = false;
        if (!dumped) {
            dumped = true;
            int raw_min = 0x7FFFFFFF, raw_max = 0;
            for (const TensorView& t : {inferer.OutputView(cls_idx_[0]),
                                        inferer.OutputView(reg_idx_[0])}) {
                if (t.dtype == DataType::kU8 || t.dtype == DataType::kI8) {
                    const uint8_t* p = static_cast<const uint8_t*>(t.data);
                    for (size_t i = 0; i < t.size_bytes; ++i) {
                        if (p[i] < raw_min) raw_min = p[i];
                        if (p[i] > raw_max) raw_max = p[i];
                    }
                }
            }
            ALG_LOGD("[det-diag] rtmdet cls/reg raw[min=%d max=%d] cand=%zu after_nms=%zu "
                     "top_score=%.4f",
                     raw_min, raw_max, props_.size(), filtered_.size(),
                     filtered_.empty() ? -1.0f : filtered_[0].score);
        }
    }

    /* 4. 反映射回原图：减去 pad 再除以 letterbox scale（与 yolox_det 一致）。 */
    const float inv_s = state.scale_ratio > 0 ? 1.0f / state.scale_ratio : 1.0f;
    const int max_x = state.original_width  - 1;
    const int max_y = state.original_height - 1;

    const bool emit_category = !category_.empty();
    out->reserve(filtered_.size());
    for (const Proposal& p : filtered_) {
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

        if (emit_category) {
            Attribute a;
            a.name = "category";
            a.value_str = category_;
            o.attributes.push_back(std::move(a));
            o.field_mask |= ALG_FIELD_ATTRIBUTES;
        }
        out->push_back(std::move(o));
    }
    return ALG_OK;
}

}  // namespace alg
