/** Common activation and classification operations for postprocessing. */
#ifndef ALG_CORE_POSTPROCESS_MATH_UTILS_H
#define ALG_CORE_POSTPROCESS_MATH_UTILS_H

#include <cmath>
#include <limits>
#include "core/tensor_utils.h"

namespace alg {

inline float Sigmoid(float x) {
    return 1.0f / (1.0f + std::exp(-x));
}

/* 用于预筛选的 logit 阈值：略向低侧放宽，避免浮点舍入在边界误剪。 */
inline float LogitThreshold(float p) {
    if (p <= 0.0f)
        return -std::numeric_limits<float>::infinity();
    if (p >= 1.0f)
        return std::numeric_limits<float>::infinity();
    return std::log(p / (1.0f - p)) - 1e-6f;
}

/* 数值稳定 softmax，返回首个最大值的相对下标及其概率。
 * 调用方保证 n > 0、[base, base + n) 为可读的连续物理元素。 */
inline void SoftmaxArgmax(const TensorView& t, size_t base, int n, int* arg, float* max_prob) {
    float m = ReadElem(t, base);
    for (int i = 1; i < n; ++i) {
        float v = ReadElem(t, base + i);
        if (v > m)
            m = v;
    }
    float sum = 0.f;
    int best = 0;
    float best_logit = ReadElem(t, base);
    for (int i = 0; i < n; ++i) {
        float v = ReadElem(t, base + i);
        sum += std::exp(v - m);
        if (v > best_logit) {
            best_logit = v;
            best = i;
        }
    }
    *arg = best;
    *max_prob = std::exp(best_logit - m) / sum;
}

}  // namespace alg

#endif  // ALG_CORE_POSTPROCESS_MATH_UTILS_H
