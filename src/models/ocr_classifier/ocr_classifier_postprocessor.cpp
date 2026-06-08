#include "models/ocr_classifier/ocr_classifier_postprocessor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

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

/* 限速字符串 → 右对齐的 P 位字符索引（缺位补 blank）+ 数值。
 * "90"→(blank,9,0) speed=90；"120"→(1,2,0) speed=120。非纯数字 / 位数超 P → 返回 false。 */
bool ParseClassDigits(const std::string& s, int num_positions, int blank,
                      std::vector<int>* digits, long* speed_out) {
    if (s.empty() || static_cast<int>(s.size()) > num_positions) return false;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    digits->assign(num_positions, blank);
    int n = static_cast<int>(s.size());
    for (int j = 0; j < n; ++j)
        (*digits)[num_positions - 1 - j] = s[n - 1 - j] - '0';
    *speed_out = std::atol(s.c_str());
    return true;
}

}  // namespace

Status OcrClassifierPostprocessor::Configure(const IInferer& inferer,
                                             const Json::Value& params) {
    num_chars_      = params.get("num_chars", 11).asInt();
    blank_index_    = params.get("blank_index", 10).asInt();
    conf_threshold_ = params.get("conf_threshold", 0.5f).asFloat();
    category_       = params.get("category", "").asString();

    class_names_.clear();
    if (params.isMember("class_names") && params["class_names"].isArray()) {
        for (Json::ArrayIndex i = 0; i < params["class_names"].size(); ++i)
            class_names_.push_back(params["class_names"][i].asString());
    }
    if (class_names_.empty()) {
        ALG_LOGE("ocr_classifier: class_names 不能为空（解码 LUT 与限速值由它推导）");
        return ALG_E_POSTPROCESS;
    }

    /* 位数（=头数）：显式配置优先，否则按 class_names 里最长的纯数字串推导。 */
    int derived_positions = 1;
    for (const auto& n : class_names_) {
        bool numeric = !n.empty();
        for (char c : n) numeric = numeric && std::isdigit(static_cast<unsigned char>(c));
        if (numeric) derived_positions = std::max(derived_positions, static_cast<int>(n.size()));
    }
    num_positions_ = params.get("num_positions", derived_positions).asInt();

    if (num_chars_ <= 0 || blank_index_ < 0 || blank_index_ >= num_chars_ ||
        num_positions_ <= 0) {
        ALG_LOGE("ocr_classifier: 参数非法 num_chars=%d blank_index=%d num_positions=%d",
                 num_chars_, blank_index_, num_positions_);
        return ALG_E_POSTPROCESS;
    }

    /* 头索引解析：优先按 name 匹配（head_names），name 不可用时退回 head_indices。
     * 顺序按"高位→低位"，对应 class_name 从左到右补齐后的位。 */
    std::vector<std::string> head_names;
    if (params.isMember("head_names") && params["head_names"].isArray())
        for (Json::ArrayIndex i = 0; i < params["head_names"].size(); ++i)
            head_names.push_back(params["head_names"][i].asString());
    std::vector<int> head_indices;
    if (params.isMember("head_indices") && params["head_indices"].isArray())
        for (Json::ArrayIndex i = 0; i < params["head_indices"].size(); ++i)
            head_indices.push_back(params["head_indices"][i].asInt());

    const int n_out = inferer.NumOutputs();
    if (n_out < num_positions_) {
        ALG_LOGE("ocr_classifier: 需要 %d 个输出头，但 NumOutputs=%d", num_positions_, n_out);
        return ALG_E_POSTPROCESS;
    }

    resolved_head_idx_.assign(num_positions_, -1);
    for (int p = 0; p < num_positions_; ++p) {
        int idx = (p < static_cast<int>(head_indices.size())) ? head_indices[p] : p;
        /* 名字匹配（鲁棒：三头同形状无法靠 size 区分，MNN 输出顺序也可能被打乱）。 */
        if (p < static_cast<int>(head_names.size()) && !head_names[p].empty()) {
            for (int i = 0; i < n_out; ++i) {
                if (inferer.OutputView(i).name == head_names[p]) { idx = i; break; }
            }
        }
        if (idx < 0 || idx >= n_out) {
            ALG_LOGE("ocr_classifier: 第 %d 位解析到非法输出索引 %d (NumOutputs=%d)",
                     p, idx, n_out);
            return ALG_E_POSTPROCESS;
        }
        if (inferer.OutputView(idx).shape.Numel() < num_chars_) {
            ALG_LOGE("ocr_classifier: 第 %d 位输出[%d] numel=%d < num_chars=%d",
                     p, idx, inferer.OutputView(idx).shape.Numel(), num_chars_);
            return ALG_E_POSTPROCESS;
        }
        resolved_head_idx_[p] = idx;
        ALG_LOGI("ocr_classifier: 第 %d 位 → output[%d] name='%s'",
                 p, idx, inferer.OutputView(idx).name.c_str());
    }

    /* 解码 LUT + 限速值表：完全由 class_names 推导，不硬编码 CLASS_TO_CHARS。 */
    long lut_size = 1;
    for (int p = 0; p < num_positions_; ++p) lut_size *= num_chars_;
    if (lut_size <= 0 || lut_size > (1L << 24)) {
        ALG_LOGE("ocr_classifier: 解码 LUT 过大 (num_chars=%d ^ num_positions=%d)",
                 num_chars_, num_positions_);
        return ALG_E_POSTPROCESS;
    }
    decode_lut_.assign(static_cast<size_t>(lut_size), -1);
    value_lut_.assign(class_names_.size(), 0);

    int n_valid = 0;
    std::vector<int> digits;
    for (size_t i = 0; i < class_names_.size(); ++i) {
        long speed = 0;
        if (!ParseClassDigits(class_names_[i], num_positions_, blank_index_, &digits, &speed)) {
            /* 非数字类（如 "other"/"background"）：不进 LUT，永不会被解码命中。 */
            value_lut_[i] = 0;
            continue;
        }
        long flat = 0;
        for (int p = 0; p < num_positions_; ++p) flat = flat * num_chars_ + digits[p];
        if (decode_lut_[static_cast<size_t>(flat)] >= 0) {
            ALG_LOGE("ocr_classifier: class_names[%zu]='%s' 与 [%d] 字符组合冲突",
                     i, class_names_[i].c_str(), decode_lut_[static_cast<size_t>(flat)]);
            return ALG_E_POSTPROCESS;
        }
        decode_lut_[static_cast<size_t>(flat)] = static_cast<int>(i);
        /* AlgSpeedLimitValue 约定 value = km/h ÷ 10（限速值都是 10 的倍数）。 */
        value_lut_[i] = (speed % 10 == 0) ? static_cast<int>(speed / 10) : 0;
        ++n_valid;
    }
    if (n_valid == 0) {
        ALG_LOGE("ocr_classifier: class_names 中无可解析的数字类，无法建立解码 LUT");
        return ALG_E_POSTPROCESS;
    }

    ALG_LOGI("ocr_classifier: ready, positions=%d chars=%d blank=%d classes=%zu valid=%d thr=%.2f",
             num_positions_, num_chars_, blank_index_, class_names_.size(), n_valid,
             conf_threshold_);
    configured_ = true;
    return ALG_OK;
}

Status OcrClassifierPostprocessor::Apply(const IInferer& inferer,
                                         const PreprocessState& /*state*/,
                                         std::vector<Object>* out) {
    if (!configured_) return ALG_E_NOT_INITIALIZED;
    if (!out) return ALG_E_INVALID_ARG;
    out->clear();

    /* 逐位 softmax+argmax，装配扁平索引，min-prob 作联合置信度。 */
    long  flat = 0;
    float min_prob = std::numeric_limits<float>::max();
    int   dbg_arg[8] = {0};
    float dbg_prob[8] = {0};
    for (int p = 0; p < num_positions_; ++p) {
        int   arg = 0;
        float prob = 0.f;
        SoftmaxArgmax(inferer.OutputView(resolved_head_idx_[p]), num_chars_, &arg, &prob);
        flat = flat * num_chars_ + arg;
        min_prob = std::min(min_prob, prob);
        if (p < 8) { dbg_arg[p] = arg; dbg_prob[p] = prob; }
    }

    int cls_id = decode_lut_[static_cast<size_t>(flat)];

    /* —— 一次性诊断（前若干个 ROI）：看 stage2 到底解出了什么、为何丢弃 ——
     * 关注：三头 argmax 是否合理(百位∈{1,blank=10}、个位应=0)、min_prob 是否过阈、
     * cls_id 是否 -1(非法组合，多半是三头顺序错位)。确认后删除。 */
    {
        static int dbg_n = 0;
        if (dbg_n < 8) {
            ++dbg_n;
            const TensorView& h0 = inferer.OutputView(resolved_head_idx_[0]);
            ALG_LOGW("[cls-diag] heads_idx=[%d,%d,%d] arg(h,t,u)=[%d,%d,%d] "
                     "prob=[%.3f,%.3f,%.3f] min_prob=%.3f thr=%.2f cls_id=%d name='%s' "
                     "h0.dtype=%d scale=%g zp=%d raw0..3=%.2f,%.2f,%.2f,%.2f",
                     resolved_head_idx_[0], resolved_head_idx_[1], resolved_head_idx_[2],
                     dbg_arg[0], dbg_arg[1], dbg_arg[2],
                     dbg_prob[0], dbg_prob[1], dbg_prob[2], min_prob, conf_threshold_, cls_id,
                     (cls_id >= 0 && cls_id < (int)class_names_.size()) ? class_names_[cls_id].c_str() : "-",
                     (int)h0.dtype, h0.quant.scale, h0.quant.zero_point,
                     ReadElem(h0, 0), ReadElem(h0, 1), ReadElem(h0, 2), ReadElem(h0, 3));
        }
    }

    /* 非法字符组合（含个位非 '0'）→ 拒识；空产出 → ChainSolution drop 掉 src 框。 */
    if (cls_id < 0) return ALG_OK;
    /* 开放集兜底：最不确定那位低于阈值 → 丢弃（字母牌 / 广告过滤）。 */
    if (min_prob < conf_threshold_) return ALG_OK;

    Object o;
    o.field_mask = ALG_FIELD_BOX | ALG_FIELD_ATTRIBUTES;
    o.label     = cls_id;
    o.box.score = min_prob;
    o.box.xmin = o.box.ymin = o.box.xmax = o.box.ymax = 0;  /* ROI 内坐标，ChainSolution 不用 */
    o.value     = value_lut_[cls_id];  /* AlgSpeedLimitValue（km/h ÷ 10） */

    Attribute attr;
    attr.name        = "class";
    attr.value_int   = cls_id;
    attr.value_float = min_prob;
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
