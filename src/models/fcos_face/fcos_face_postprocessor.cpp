#include "models/fcos_face/fcos_face_postprocessor.h"

#include <algorithm>
#include <cmath>

#include "core/logger.h"
#include "core/postprocess/nms.h"

#ifdef __aarch64__
#include <arm_neon.h>
#endif

namespace alg {

namespace {

/*
 * 关键优化：DecodeLevel 不再算 sigmoid，只存"raw 反 sigmoid 之前"的值。
 * 因为：
 *   - 阈值过滤已经在 uint8 域提前完成（u8_thresh）；
 *   - NMS 排序对 score 是单调函数（raw 与 sigmoid 同序），结果一致；
 *   - 真正需要写到 Object.box.score 的只有 NMS 后的"幸存者"。
 * 这样 expf 调用数从 "所有过阈值的 cell" 降到 "NMS 后剩下的 N 个"。
 */
void DecodeLevel(std::vector<Proposal>& props, const TensorView& cls, const TensorView& reg,
                 unsigned stride, float raw_thresh) {
    const int W = cls.W();
    const int H = cls.H();
    const float fs = static_cast<float>(stride);
    const int HW = H * W;

    const bool cls_u8 = (cls.dtype == DataType::kU8);
    const bool reg_u8 = (reg.dtype == DataType::kU8);

    if (cls_u8 && reg_u8) {
        const uint8_t* cls_d = static_cast<const uint8_t*>(cls.data);
        const uint8_t* reg_d = static_cast<const uint8_t*>(reg.data);
        const float cls_scale = cls.quant.scale;
        const int   cls_zp = cls.quant.zero_point;
        const float reg_scale = reg.quant.scale;
        const int   reg_zp = reg.quant.zero_point;

        /* 整型阈值：避免每 cell 做浮点比较。 */
        int thr_i = static_cast<int>(raw_thresh / cls_scale + cls_zp);
        if (thr_i >= 255) return;
        const uint8_t thr = static_cast<uint8_t>(thr_i < 0 ? 0 : thr_i);

        const uint8_t* rp[4] = {reg_d, reg_d + HW, reg_d + 2 * HW, reg_d + 3 * HW};

#ifdef __aarch64__
        /* NEON 块级早退：每 16 个 cell 用一次 vmaxvq_u8 看最大值是否过阈值，
         * 没过就整块跳过 16 个 cell。人脸检测过阈值率通常 < 1%，块级跳过显著
         * 减少分支与单 byte 访存。 */
        const uint8x16_t v_thr = vdupq_n_u8(thr);
#endif
        for (int h = 0; h < H; ++h) {
            const float cy = (h + 0.5f) * fs;
            const int row = h * W;
            int w = 0;
#ifdef __aarch64__
            for (; w + 16 <= W; w += 16) {
                uint8x16_t v = vld1q_u8(cls_d + row + w);
                /* 如果整块 max ≤ thr 则全部跳过；否则 fall through 到 scalar */
                if (vmaxvq_u8(v) <= thr) continue;
                /* 块内有候选，逐 cell 处理。 */
                for (int k = 0; k < 16; ++k) {
                    const int idx = row + w + k;
                    if (cls_d[idx] <= thr) continue;
                    float cls_raw = (cls_d[idx] - cls_zp) * cls_scale;
                    float l = (rp[0][idx] - reg_zp) * reg_scale;
                    float t = (rp[1][idx] - reg_zp) * reg_scale;
                    float r = (rp[2][idx] - reg_zp) * reg_scale;
                    float b = (rp[3][idx] - reg_zp) * reg_scale;
                    float cx = (w + k + 0.5f) * fs;
                    Proposal p{cx - l * fs, cy - t * fs, cx + r * fs, cy + b * fs,
                               cls_raw, 0, 0};
                    props.push_back(p);
                }
            }
#endif
            for (; w < W; ++w) {
                const int idx = row + w;
                if (cls_d[idx] <= thr) continue;
                float cls_raw = (cls_d[idx] - cls_zp) * cls_scale;
                float l = (rp[0][idx] - reg_zp) * reg_scale;
                float t = (rp[1][idx] - reg_zp) * reg_scale;
                float r = (rp[2][idx] - reg_zp) * reg_scale;
                float b = (rp[3][idx] - reg_zp) * reg_scale;
                float cx = (w + 0.5f) * fs;
                Proposal p{cx - l * fs, cy - t * fs, cx + r * fs, cy + b * fs,
                           cls_raw, 0, 0};
                props.push_back(p);
            }
        }
        return;
    }

    const float* cls_f = static_cast<const float*>(cls.data);
    const float* reg_f = static_cast<const float*>(reg.data);
    for (int h = 0; h < H; ++h) {
        const float cy = (h + 0.5f) * fs;
        const int row = h * W;
        for (int w = 0; w < W; ++w) {
            float cls_raw = cls_f[row + w];
            if (cls_raw <= raw_thresh) continue;
            int idx = row + w;
            float l = reg_f[0 * HW + idx];
            float t = reg_f[1 * HW + idx];
            float r = reg_f[2 * HW + idx];
            float b = reg_f[3 * HW + idx];
            float cx = (w + 0.5f) * fs;
            Proposal p{cx - l * fs, cy - t * fs, cx + r * fs, cy + b * fs,
                       cls_raw, 0, 0};
            props.push_back(p);
        }
    }
}

inline float Sigmoid(float x) {
    return 1.0f / (1.0f + std::exp(-x));
}

}  // namespace

Status FcosFacePostprocessor::Configure(const IInferer& inferer, const Json::Value& params) {
    conf_threshold_ = params.get("conf_threshold", 0.6f).asFloat();
    nms_threshold_  = params.get("nms_threshold",  0.4f).asFloat();
    num_levels_     = params.get("num_levels", 4).asInt();
    if (params.isMember("strides") && params["strides"].isArray()) {
        strides_.clear();
        for (Json::ArrayIndex i = 0; i < params["strides"].size(); ++i)
            strides_.push_back(params["strides"][i].asInt());
        num_levels_ = static_cast<int>(strides_.size());
    }
    if (inferer.NumOutputs() < num_levels_ * 2) {
        ALG_LOGE("FcosFacePostprocessor: expected %d outputs, got %d",
                 num_levels_ * 2, inferer.NumOutputs());
        return ALG_E_POSTPROCESS;
    }
    configured_ = true;
    return ALG_OK;
}

Status FcosFacePostprocessor::Apply(const IInferer& inferer, const PreprocessState& state,
                                    std::vector<Object>* out) {
    if (!configured_) return ALG_E_NOT_INITIALIZED;
    if (!out) return ALG_E_INVALID_ARG;
    out->clear();

    const float raw_thresh = std::log(conf_threshold_ / (1.0f - conf_threshold_));

    props_.clear();
    if (props_.capacity() < 256) props_.reserve(256);

    for (int i = 0; i < num_levels_; ++i) {
        DecodeLevel(props_, inferer.OutputView(i), inferer.OutputView(i + num_levels_),
                    static_cast<unsigned>(strides_[i]), raw_thresh);
    }
    Nms(props_, nms_threshold_, &nms_scratch_);

    const float inv_s = state.scale_ratio > 0 ? 1.0f / state.scale_ratio : 1.0f;
    const int max_x = state.original_width - 1;
    const int max_y = state.original_height - 1;

    out->reserve(props_.size());
    for (const auto& p : props_) {
        Object o;
        o.field_mask = ALG_FIELD_BOX;
        /* 只对幸存者做 sigmoid。 */
        o.box.score = Sigmoid(p.score);
        o.box.label = 0;
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
