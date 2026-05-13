/**
 * @file crop_util.h
 * @brief 通用裁剪工具。
 *
 * 性能契约（重要）：
 * - 原图只解码一次：调用方先用 DecodeSourceToBgrInto() 把整帧 AlgImage 解到一份
 *   cv::Mat（共享或拷贝由解码路径决定），然后**多次**调用 CropFromDecoded()，
 *   每次只做 ROI 视图（零拷贝）。
 * - 输出 AlgImage 携带正确 stride，下游 preprocessor 必须按 stride 读取。
 */

#ifndef ALG_CORE_SOLUTION_CROP_UTIL_H
#define ALG_CORE_SOLUTION_CROP_UTIL_H

#include <opencv2/core/mat.hpp>

#include "alg_types.h"
#include "core/config/config.h"

namespace alg {

/* crop 坐标 → 原图坐标：x_orig = x_crop + offset_x。 */
struct CropTransform {
    int offset_x = 0;
    int offset_y = 0;
    int crop_w   = 0;
    int crop_h   = 0;
};

/**
 * 把 AlgImage 解码为 BGR cv::Mat，写入调用方持有的 `out`。
 *
 * - BGR：零拷贝包装 src.data，共享内存。out 仅作为头返回。
 * - RGB/GRAY/NV12/NV21：必然一次 cvtColor；OpenCV 会复用 out 的底层 buffer
 *   （同 shape/type 多次调用时不再分配）。
 *
 * 调用方需保证 src.data 的存活期覆盖所有 ROI 视图的使用期。
 */
void DecodeSourceToBgrInto(const AlgImage& src, cv::Mat* out);

/**
 * 在已解码的 BGR Mat 上按 box + crop config 抠出 ROI。
 * `holder` 接收**非拷贝**的 ROI Mat（共享 decoded 的像素），`out` 是携带
 * stride 的 AlgImage 视图。
 */
bool CropFromDecoded(const cv::Mat& decoded_bgr, const AlgBox& box,
                     const CropConfig& cfg,
                     cv::Mat* holder, AlgImage* out, CropTransform* xf);

}  // namespace alg

#endif  // ALG_CORE_SOLUTION_CROP_UTIL_H
