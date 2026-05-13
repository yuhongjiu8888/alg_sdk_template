#include "models/pfld_landmark/pfld_landmark_postprocessor.h"

#include <algorithm>

#include "core/logger.h"

namespace alg {

Status PfldLandmarkPostprocessor::Configure(const IInferer& inferer, const Json::Value& params) {
    num_points_  = params.get("num_points", 5).asInt();
    interleaved_ = params.get("interleaved", true).asBool();
    if (num_points_ <= 0) {
        ALG_LOGE("PfldLandmark: num_points must be > 0"); return ALG_E_POSTPROCESS;
    }
    if (inferer.NumOutputs() < 1) {
        ALG_LOGE("PfldLandmark: need at least 1 output"); return ALG_E_POSTPROCESS;
    }
    configured_ = true;
    return ALG_OK;
}

namespace {
float ReadElem(const TensorView& t, int idx) {
    if (t.dtype == DataType::kF32) return static_cast<const float*>(t.data)[idx];
    if (t.dtype == DataType::kU8) {
        const uint8_t* p = static_cast<const uint8_t*>(t.data);
        return (p[idx] - t.quant.zero_point) * t.quant.scale;
    }
    return 0.0f;
}
}  // namespace

Status PfldLandmarkPostprocessor::Apply(const IInferer& inferer, const PreprocessState& state,
                                        std::vector<Object>* out) {
    if (!configured_) return ALG_E_NOT_INITIALIZED;
    out->clear();

    const TensorView& t = inferer.OutputView(0);
    const float inv_s = state.scale_ratio > 0 ? 1.0f / state.scale_ratio : 1.0f;

    Object o;
    o.field_mask = ALG_FIELD_KEYPOINTS;
    o.keypoints.x.resize(num_points_);
    o.keypoints.y.resize(num_points_);

    /* 假设网络输出已经是"喂给本模型那张图"的像素坐标（含 letterbox 的 padding）。
     * 若实际是 [0,1] 归一化坐标，应用方可以在 JSON 里加 "normalized": true 然后
     * 在这里乘上网络输入尺寸（PFLD 在不同实现里都见过，这里给最常见的那种）。 */
    for (int i = 0; i < num_points_; ++i) {
        float xn, yn;
        if (interleaved_) {
            xn = ReadElem(t, 2 * i + 0);
            yn = ReadElem(t, 2 * i + 1);
        } else {
            xn = ReadElem(t, i);
            yn = ReadElem(t, num_points_ + i);
        }
        /* 假设网络输出已经是 [0, net_in_w] 像素坐标（PFLD 常见做法）。若实际是
         * 归一化到 [0, 1]，在 JSON 里加一项 "normalized": true 并乘以 net 大小即可。*/
        float xp = xn - state.pad_left;
        float yp = yn - state.pad_top;
        o.keypoints.x[i] = std::max(0.0f, xp * inv_s);
        o.keypoints.y[i] = std::max(0.0f, yp * inv_s);
    }
    out->push_back(std::move(o));
    return ALG_OK;
}

}  // namespace alg
