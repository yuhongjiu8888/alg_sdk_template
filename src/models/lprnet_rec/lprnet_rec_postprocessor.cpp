#include "models/lprnet_rec/lprnet_rec_postprocessor.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/logger.h"

namespace alg {

namespace {

/* 反量化辅助：读取 t.data 在 byte_ptr 处的一个元素（按 dtype 解包，
 * 支持 FLOAT / FLOAT16 / 量化整型）。与其它后处理器 ReadElem 等价，
 * 只是按字节指针定位 —— 便于支撑 ATC 输出按行对齐（row_stride）的张量。 */
inline float ReadElemAt(const TensorView& t, const uint8_t* p) {
    switch (t.dtype) {
        case DataType::kF32: {
            float v;
            std::memcpy(&v, p, sizeof(v));
            return v;
        }
        case DataType::kI8: {
            int8_t raw;
            std::memcpy(&raw, p, sizeof(raw));
            return (static_cast<int>(raw) - t.quant.zero_point) * t.quant.scale;
        }
        case DataType::kU8: {
            uint8_t raw;
            std::memcpy(&raw, p, sizeof(raw));
            return (static_cast<int>(raw) - t.quant.zero_point) * t.quant.scale;
        }
        case DataType::kI16: {
            int16_t raw;
            std::memcpy(&raw, p, sizeof(raw));
            return (static_cast<int>(raw) - t.quant.zero_point) * t.quant.scale;
        }
        case DataType::kU16: {
            uint16_t raw;
            std::memcpy(&raw, p, sizeof(raw));
            return (static_cast<int>(raw) - t.quant.zero_point) * t.quant.scale;
        }
        case DataType::kI32: {
            int32_t raw;
            std::memcpy(&raw, p, sizeof(raw));
            return (static_cast<float>(raw) - static_cast<float>(t.quant.zero_point)) *
                   t.quant.scale;
        }
        case DataType::kF16: {
            uint16_t h;
            std::memcpy(&h, p, sizeof(h));
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

Status LprnetRecPostprocessor::Configure(const IInferer& inferer, const Json::Value& params) {
    characters_     = params.get("characters", "").asString();
    blank_index_    = params.get("blank_index", 36).asInt();
    timesteps_      = params.get("timesteps", 32).asInt();
    channels_       = params.get("channels", 0).asInt();
    conf_threshold_ = params.get("conf_threshold", 0.0f).asFloat();
    category_       = params.get("category", "").asString();

    if (characters_.empty()) {
        ALG_LOGE("lprnet_rec: characters 不能为空");
        return ALG_E_POSTPROCESS;
    }
    if (channels_ <= 0) channels_ = static_cast<int>(characters_.size()) + 1;
    if (timesteps_ <= 0 || channels_ <= blank_index_ || blank_index_ < 0) {
        ALG_LOGE("lprnet_rec: 参数非法 timesteps=%d channels=%d blank_index=%d",
                 timesteps_, channels_, blank_index_);
        return ALG_E_POSTPROCESS;
    }
    if (inferer.NumOutputs() < 1) {
        ALG_LOGE("lprnet_rec: need at least 1 output");
        return ALG_E_POSTPROCESS;
    }
    const TensorView& t = inferer.OutputView(0);
    if (t.shape.Numel() < timesteps_ * channels_) {
        ALG_LOGE("lprnet_rec: output numel=%d < timesteps*channels=%d",
                 t.shape.Numel(), timesteps_ * channels_);
        return ALG_E_POSTPROCESS;
    }
    /* 按行对齐输出（row_stride>0）时，逐行字节容量也要够。 */
    {
        const size_t row_bytes = t.row_stride
            ? t.row_stride
            : static_cast<size_t>(channels_) * BytesPerElement(t.dtype);
        if (t.size_bytes < static_cast<size_t>(timesteps_) * row_bytes) {
            ALG_LOGE("lprnet_rec: output size=%zu < timesteps*row_bytes=%zu",
                     t.size_bytes, static_cast<size_t>(timesteps_) * row_bytes);
            return ALG_E_POSTPROCESS;
        }
    }

    ALG_LOGI("lprnet_rec: ready, chars=%zu timesteps=%d channels=%d blank=%d thr=%.2f cat='%s'",
             characters_.size(), timesteps_, channels_, blank_index_, conf_threshold_,
             category_.c_str());
    configured_ = true;
    return ALG_OK;
}

Status LprnetRecPostprocessor::Apply(const IInferer& inferer,
                                     const PreprocessState& /*state*/,
                                     std::vector<Object>* out) {
    if (!configured_) return ALG_E_NOT_INITIALIZED;
    if (!out) return ALG_E_INVALID_ARG;
    out->clear();

    const TensorView& t = inferer.OutputView(0);

    /* CTC greedy 解码：逐时间步 softmax+argmax → 去重、去 blank。
     * 输出按 [t][c] 时间步主序（与 license demo 读取一致）。ATC 输出可能按行
     * 对齐（row_stride > 0，如 1x32x37 FLOAT 侦测到 160B/行、密集仅 148B），
     * 每行只读前 channels 个元素。 */
    const size_t elem_bytes = static_cast<size_t>(BytesPerElement(t.dtype));
    const size_t row_bytes = t.row_stride ? t.row_stride : channels_ * elem_bytes;
    const uint8_t* base_ptr = static_cast<const uint8_t*>(t.data);
    std::string text;
    float score = 1.0f;
    int previous = blank_index_;
    int dbg_max_t = -1, dbg_max_c = -1;
    float dbg_max_logit = -1e30f;
    for (int ts = 0; ts < timesteps_; ++ts) {
        const uint8_t* row = base_ptr + static_cast<size_t>(ts) * row_bytes;
        float m = ReadElemAt(t, row);
        for (int c = 1; c < channels_; ++c) {
            float v = ReadElemAt(t, row + static_cast<size_t>(c) * elem_bytes);
            if (v > m) m = v;
        }
        float sum = 0.0f;
        int best = 0;
        float best_logit = ReadElemAt(t, row);
        for (int c = 0; c < channels_; ++c) {
            const float v = ReadElemAt(t, row + static_cast<size_t>(c) * elem_bytes);
            if (v > best_logit) { best_logit = v; best = c; }
            sum += std::exp(v - m);
        }
        if (best_logit > dbg_max_logit) {
            dbg_max_logit = best_logit;
            dbg_max_t = ts;
            dbg_max_c = best;
        }
        const float best_prob = std::exp(best_logit - m) / (sum + 1e-9f);
        if (best != previous && best != blank_index_ &&
            best >= 0 && best < static_cast<int>(characters_.size())) {
            text += characters_[best];
            score *= best_prob;
        }
        previous = best;
    }

    /* —— 一次性诊断（前 8 个 ROI）：看 stage2 解出什么文本、置信度如何。 */
    {
        static int dbg_n = 0;
        if (ALG_LOG_IS_ENABLED(alg::log::Level::Debug) && dbg_n < 8) {
            ++dbg_n;
            char shape[64] = {0};
            int sn = 0;
            for (int i = 0; i < t.shape.ndims && sn < 60; ++i)
                sn += snprintf(shape + sn, sizeof(shape) - sn, "%s%d",
                               i ? "x" : "", t.shape.dims[i]);
            int raw_min = 0x7FFFFFFF, raw_max = 0;
            if (t.dtype == DataType::kU8 || t.dtype == DataType::kI8) {
                const uint8_t* p = static_cast<const uint8_t*>(t.data);
                for (size_t i = 0; i < t.size_bytes; ++i) {
                    if (p[i] < raw_min) raw_min = p[i];
                    if (p[i] > raw_max) raw_max = p[i];
                }
            }
            ALG_LOGD("[cls-diag] lprnet '%s' shape=%s numel=%d raw[min=%d max=%d] "
                     "text='%s' rec_score=%.4f max_logit=%.2f@(t=%d,c=%d)",
                     t.name.c_str(), shape, t.shape.Numel(), raw_min, raw_max,
                     text.c_str(), score, dbg_max_logit, dbg_max_t, dbg_max_c);
        }
    }

    /* 置信度过滤（可选）：低于阈值 → 返回空 → classify_into drop 掉该检测框。
     * 0 = 不过滤，行为与 license demo 一致（demo 对任何结果都保留）。 */
    if (conf_threshold_ > 0.0f && score < conf_threshold_) return ALG_OK;

    Object o;
    o.field_mask = ALG_FIELD_BOX | ALG_FIELD_ATTRIBUTES;
    o.label      = -1;             /* 非检测类，label 不用 */
    o.box.score  = score;          /* 识别置信度；classify_into 再乘 det */
    o.box.xmin = o.box.ymin = o.box.xmax = o.box.ymax = 0;  /* ROI 内坐标，ChainSolution 不用 */

    Attribute cat;
    cat.name = "category";
    cat.value_str = category_;
    o.attributes.push_back(std::move(cat));

    Attribute txt;
    txt.name = "text";
    txt.value_str = text;
    o.attributes.push_back(std::move(txt));

    Attribute rec;
    rec.name = "rec_score";
    rec.value_float = score;
    o.attributes.push_back(std::move(rec));

    out->push_back(std::move(o));
    return ALG_OK;
}

}  // namespace alg
