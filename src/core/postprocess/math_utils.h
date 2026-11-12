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

/* read_logit 负责张量布局和读取方式；返回减去最大 logit 后的指数和。 */
template <typename ReadLogit>
inline float SoftmaxSum(ReadLogit read_logit, int n, int* arg, float* max_logit) {
    float m = read_logit(0);
    int best = 0;
    for (int i = 1; i < n; ++i) {
        float v = read_logit(i);
        if (v > m) {
            m = v;
            best = i;
        }
    }
    float sum = 0.f;
    for (int i = 0; i < n; ++i)
        sum += std::exp(read_logit(i) - m);
    *arg = best;
    *max_logit = m;
    return sum;
}

/* 数值稳定 softmax，返回首个最大值的相对下标及其概率。
 * 调用方保证 n > 0、[base, base + n) 为可读的连续物理元素。 */
inline void SoftmaxArgmax(const TensorView& t, size_t base, int n, int* arg, float* max_prob) {
    float max_logit;
    const float sum = SoftmaxSum([&](int i) { return ReadElem(t, base + i); }, n, arg, &max_logit);
    *max_prob = 1.0f / sum;
}

}  // namespace alg

#endif  // ALG_CORE_POSTPROCESS_MATH_UTILS_H
