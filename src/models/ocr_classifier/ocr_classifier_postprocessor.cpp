#include "models/ocr_classifier/ocr_classifier_postprocessor.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include "core/logger.h"
#include "core/postprocess/math_utils.h"

namespace alg {

namespace {

/* 门控向量 softmax：返回 argmax、P(限速)、P(禁停)。base=门控在张量内的起始偏移。 */
void GateProbs(
    const TensorView& t, int base, int n, int speed_idx, int nopark_idx, int* arg, float* p_speed, float* p_nopark) {
    float m;
    const float sum = SoftmaxSum([&](int i) { return ReadElem(t, base + i); }, n, arg, &m);
    const float inv = 1.0f / (sum + 1e-9f);
    *p_speed = std::exp(ReadElem(t, base + speed_idx) - m) * inv;
    *p_nopark = std::exp(ReadElem(t, base + nopark_idx) - m) * inv;
}

/* 限速字符串 → 右对齐的 P 位字符索引（缺位补 blank）+ 数值。
 * "90"→(blank,9,0) speed=90；"120"→(1,2,0) speed=120。非纯数字 / 位数超 P → 返回 false。 */
bool ParseClassDigits(const std::string& s, int num_positions, int blank, std::vector<int>* digits, long* speed_out) {
    if (s.empty() || static_cast<int>(s.size()) > num_positions)
        return false;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return false;
    digits->assign(num_positions, blank);
    int n = static_cast<int>(s.size());
    for (int j = 0; j < n; ++j)
        (*digits)[num_positions - 1 - j] = s[n - 1 - j] - '0';
    *speed_out = std::atol(s.c_str());
    return true;
}

}  // namespace

Status OcrClassifierPostprocessor::Configure(const IInferer& inferer, const Json::Value& params) {
    num_chars_ = params.get("num_chars", 11).asInt();
    blank_index_ = params.get("blank_index", 10).asInt();
    conf_threshold_ = params.get("conf_threshold", 0.5f).asFloat();
    category_ = params.get("category", "").asString();

    /* v3.5 门控（可选，默认 0=无门控纯 OCR）。 */
    gate_channels_ = params.get("gate_channels", 0).asInt();
    gate_speed_index_ = params.get("gate_speed_index", 1).asInt();
    gate_nopark_index_ = params.get("gate_nopark_index", 2).asInt();
    gate_threshold_ = params.get("gate_threshold", 0.5f).asFloat();
    nopark_category_ = params.get("nopark_category", "").asString();
    nopark_sign_value_ = params.get("nopark_sign_value", 0).asInt();

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
        for (char c : n)
            numeric = numeric && std::isdigit(static_cast<unsigned char>(c));
        if (numeric)
            derived_positions = std::max(derived_positions, static_cast<int>(n.size()));
    }
    num_positions_ = params.get("num_positions", derived_positions).asInt();

    if (num_chars_ <= 0 || blank_index_ < 0 || blank_index_ >= num_chars_ || num_positions_ <= 0) {
        ALG_LOGE("ocr_classifier: 参数非法 num_chars=%d blank_index=%d num_positions=%d", num_chars_, blank_index_,
                 num_positions_);
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

    /* 单输出模式：模型只出一个张量，P 个位连续切片（避开 XMM 多输出+CPU 切图的
     * 板端坑）。各位都读 output[0]，起始偏移 p*num_chars 在 Apply 里算。 */
    single_output_ = (n_out == 1 && num_positions_ > 1);
    if (single_output_) {
        const int need = num_positions_ * num_chars_ + gate_channels_;
        if (inferer.OutputView(0).shape.Numel() < need) {
            ALG_LOGE("ocr_classifier: 单输出 numel=%d < positions*chars(+gate)=%d", inferer.OutputView(0).shape.Numel(),
                     need);
            return ALG_E_POSTPROCESS;
        }
        resolved_head_idx_.assign(num_positions_, 0);
        ALG_LOGI("ocr_classifier: 单输出模式 output[0] numel=%d，按 %d 位 × %d 字符连续切片（gate=%d）",
                 inferer.OutputView(0).shape.Numel(), num_positions_, num_chars_, gate_channels_);
    } else {
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
                    if (inferer.OutputView(i).name == head_names[p]) {
                        idx = i;
                        break;
                    }
                }
            }
            if (idx < 0 || idx >= n_out) {
                ALG_LOGE("ocr_classifier: 第 %d 位解析到非法输出索引 %d (NumOutputs=%d)", p, idx, n_out);
                return ALG_E_POSTPROCESS;
            }
            if (inferer.OutputView(idx).shape.Numel() < num_chars_) {
                ALG_LOGE("ocr_classifier: 第 %d 位输出[%d] numel=%d < num_chars=%d", p, idx,
                         inferer.OutputView(idx).shape.Numel(), num_chars_);
                return ALG_E_POSTPROCESS;
            }
            resolved_head_idx_[p] = idx;
            ALG_LOGI("ocr_classifier: 第 %d 位 → output[%d] name='%s'", p, idx, inferer.OutputView(idx).name.c_str());
        }
    }

    /* v3.5 门控位置解析：单输出 → output[0] 内偏移 P*num_chars；多输出 → 独立门控头。 */
    gate_view_idx_ = -1;
    gate_base_ = 0;
    if (gate_channels_ > 0) {
        if (gate_speed_index_ < 0 || gate_speed_index_ >= gate_channels_ || gate_nopark_index_ < 0 ||
            gate_nopark_index_ >= gate_channels_) {
            ALG_LOGE("ocr_classifier: gate 下标越界 speed=%d nopark=%d channels=%d", gate_speed_index_,
                     gate_nopark_index_, gate_channels_);
            return ALG_E_POSTPROCESS;
        }
        if (single_output_) {
            gate_view_idx_ = 0;
            gate_base_ = num_positions_ * num_chars_; /* OCR 段之后 */
        } else {
            /* 多输出：门控是独立头，优先按 name（gate_head_name），否则索引（默认 = P，即 OCR 头之后）。 */
            int gidx = params.get("gate_head_index", num_positions_).asInt();
            std::string gname = params.get("gate_head_name", "").asString();
            if (!gname.empty())
                for (int i = 0; i < n_out; ++i)
                    if (inferer.OutputView(i).name == gname) {
                        gidx = i;
                        break;
                    }
            if (gidx < 0 || gidx >= n_out || inferer.OutputView(gidx).shape.Numel() < gate_channels_) {
                ALG_LOGE("ocr_classifier: 门控头解析失败 idx=%d name='%s' n_out=%d", gidx, gname.c_str(), n_out);
                return ALG_E_POSTPROCESS;
            }
            gate_view_idx_ = gidx;
            gate_base_ = 0;
        }
        if (nopark_sign_value_ != 0 && nopark_category_.empty()) {
            ALG_LOGE("ocr_classifier: 配了 nopark_sign_value 却没 nopark_category（FillAlgResult 无法分桶）");
            return ALG_E_POSTPROCESS;
        }
        ALG_LOGI("ocr_classifier: 门控就绪 channels=%d view=%d base=%d thr=%.2f nopark_cat='%s' val=%d", gate_channels_,
                 gate_view_idx_, gate_base_, gate_threshold_, nopark_category_.c_str(), nopark_sign_value_);
    }

    /* 解码 LUT + 限速值表：完全由 class_names 推导，不硬编码 CLASS_TO_CHARS。 */
    long lut_size = 1;
    for (int p = 0; p < num_positions_; ++p)
        lut_size *= num_chars_;
    if (lut_size <= 0 || lut_size > (1L << 24)) {
        ALG_LOGE("ocr_classifier: 解码 LUT 过大 (num_chars=%d ^ num_positions=%d)", num_chars_, num_positions_);
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
        for (int p = 0; p < num_positions_; ++p)
            flat = flat * num_chars_ + digits[p];
        if (decode_lut_[static_cast<size_t>(flat)] >= 0) {
            ALG_LOGE("ocr_classifier: class_names[%zu]='%s' 与 [%d] 字符组合冲突", i, class_names_[i].c_str(),
                     decode_lut_[static_cast<size_t>(flat)]);
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

    ALG_LOGI("ocr_classifier: ready, positions=%d chars=%d blank=%d classes=%zu valid=%d thr=%.2f", num_positions_,
             num_chars_, blank_index_, class_names_.size(), n_valid, conf_threshold_);
    configured_ = true;
    return ALG_OK;
}

Status OcrClassifierPostprocessor::Apply(const IInferer& inferer,
                                         const PreprocessState& /*state*/,
                                         std::vector<Object>* out) {
    if (!configured_)
        return ALG_E_NOT_INITIALIZED;
    if (!out)
        return ALG_E_INVALID_ARG;
    out->clear();

    /* v3.5 门控 3 路分拣（对齐 infer_onnx.py / deploy_src）：
     *   argmax==no_parking → 直接产出禁停正类（category=nopark_category，跳过 OCR）；
     *   argmax==speed 且 P(限速)≥gate_thr → 走下面 OCR；
     *   其余(other / P(限速)低) → 拒识（返回空 → ChainSolution drop 掉 src 框）。
     * nopark_min_size 需原图框尺寸，由 ChainSolution 的 min_box_short 施加（此处拿不到 box）。 */
    if (gate_channels_ > 0) {
        int gate_id = 0;
        float p_speed = 0.f, p_nopark = 0.f;
        GateProbs(inferer.OutputView(gate_view_idx_), gate_base_, gate_channels_, gate_speed_index_, gate_nopark_index_,
                  &gate_id, &p_speed, &p_nopark);
        if (gate_id == gate_nopark_index_) {
            Object np;
            np.field_mask = ALG_FIELD_BOX | ALG_FIELD_ATTRIBUTES;
            np.label = -1;           /* 非限速类，label 不用 */
            np.box.score = p_nopark; /* 联合前的 gate 置信；ChainSolution 再乘 det */
            np.box.xmin = np.box.ymin = np.box.xmax = np.box.ymax = 0;
            np.value = nopark_sign_value_; /* AlgSignType（SIGN_NO_PARKING） */
            Attribute cat;
            cat.name = "category";
            cat.value_str = nopark_category_; /* FillAlgResult 据此分桶到 signs[] */
            np.attributes.push_back(std::move(cat));
            out->push_back(std::move(np));
            return ALG_OK;
        }
        if (gate_id != gate_speed_index_ || p_speed < gate_threshold_)
            return ALG_OK; /* other / 非限速 → 拒识 */
    }

    /* 逐位 softmax+argmax，装配扁平索引，min-prob 作联合置信度。 */
    long flat = 0;
    float min_prob = std::numeric_limits<float>::max();
    for (int p = 0; p < num_positions_; ++p) {
        int arg = 0;
        float prob = 0.f;
        /* 单输出：同一张量内偏移 p*num_chars；多头：各自张量偏移 0。 */
        const int base = single_output_ ? p * num_chars_ : 0;
        SoftmaxArgmax(inferer.OutputView(resolved_head_idx_[p]), base, num_chars_, &arg, &prob);
        flat = flat * num_chars_ + arg;
        min_prob = std::min(min_prob, prob);
    }

    int cls_id = decode_lut_[static_cast<size_t>(flat)];

    /* 非法字符组合（含个位非 '0'）→ 拒识；空产出 → ChainSolution drop 掉 src 框。 */
    if (cls_id < 0)
        return ALG_OK;
    /* 开放集兜底：最不确定那位低于阈值 → 丢弃（字母牌 / 广告过滤）。 */
    if (min_prob < conf_threshold_)
        return ALG_OK;

    Object o;
    o.field_mask = ALG_FIELD_BOX | ALG_FIELD_ATTRIBUTES;
    o.label = cls_id;
    o.box.score = min_prob;
    o.box.xmin = o.box.ymin = o.box.xmax = o.box.ymax = 0; /* ROI 内坐标，ChainSolution 不用 */
    o.value = value_lut_[cls_id];                          /* AlgSpeedLimitValue（km/h ÷ 10） */

    Attribute attr;
    attr.name = "class";
    attr.value_int = cls_id;
    attr.value_float = min_prob;
    if (cls_id >= 0 && cls_id < static_cast<int>(class_names_.size()))
        attr.value_str = class_names_[cls_id];
    o.attributes.push_back(std::move(attr));

    if (!category_.empty()) {
        Attribute c;
        c.name = "category";
        c.value_str = category_;
        o.attributes.push_back(std::move(c));
    }

    out->push_back(std::move(o));
    return ALG_OK;
}

}  // namespace alg
