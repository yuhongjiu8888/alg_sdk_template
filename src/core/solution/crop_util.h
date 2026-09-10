/**
 * @file crop_util.h
 * @brief 通用裁剪工具。
 *
 * 性能契约（重要）：
 * - BGR/RGB/GRAY 可先解码一次，再通过 CropFromDecoded() 建立 ROI 视图。
 * - NV12/NV21 通过 CropFromNV12() 只转换小块 ROI，避免整帧 YUV 转色。
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
 * - BGR：零拷贝包装 src.data/plane_data[0]，共享内存。out 仅作为头返回。
 * - RGB/GRAY/NV12/NV21：必然一次 cvtColor；OpenCV 会复用 out 的底层 buffer
 *   （同 shape/type 多次调用时不再分配）。
 *
 * 调用方需保证所有输入平面的存活期覆盖所有 ROI 视图的使用期。
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

/**
 * 直接从 NV12/NV21 原图抠 ROI 子图转成 BGR（替代"全帧解码 + CropFromDecoded"）。
 *
 * 调用场景：整帧是 NV12/NV21 时，若每个子模型都做一次全帧 YUV→BGR 解码会白耗
 * 大量 CPU（1080p 一次 ~25ms）。此函数把解码范围收窄到框周边：从 Y/UV 平面各取
 * 子图视图（step=原图宽 W，非连续），再对**小区域**做 cvtColorTwoPlane。
 *
 * 注意：NV12 色度为半分辨率，子图坐标在函数内做偶对齐（clamp 分支）或偶对齐后
 * 拷贝（padded 分支，对齐偏移由 pad 填充兜底），与 CropFromDecoded 输出语义一致
 * （BGR + stride + xf 偏移映射回原图）。
 */
bool CropFromNV12(const AlgImage& src, const AlgBox& box,
                  const CropConfig& cfg,
                  cv::Mat* holder, AlgImage* out, CropTransform* xf);

}  // namespace alg

#endif  // ALG_CORE_SOLUTION_CROP_UTIL_H
