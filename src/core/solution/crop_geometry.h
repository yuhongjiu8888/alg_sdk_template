/** NV12/NV21 ROI geometry shared by CPU and hardware preprocessing. */
#ifndef ALG_CORE_SOLUTION_CROP_GEOMETRY_H
#define ALG_CORE_SOLUTION_CROP_GEOMETRY_H

#include "alg_types.h"
#include "core/config/config.h"

namespace alg {

/* crop 坐标 → 原图坐标：x_orig = x_crop + offset_x。 */
struct CropTransform {
    int offset_x = 0;
    int offset_y = 0;
    int crop_w = 0;
    int crop_h = 0;
};

/* NV12/NV21 ROI 的完整几何：扩边画布 + 图内有效区域。
 * 供 CropFromNV12 和“NV12 ROI 直接缩放”的融合前处理共用。 */
struct CropGeometry {
    int bx1 = 0, by1 = 0, bx2 = 0, by2 = 0; /* 扩边后的裁剪画布（可能越界） */
    int ix1 = 0, iy1 = 0, ix2 = 0, iy2 = 0; /* 图内有效 ROI（偶对齐） */
    int offset_x = 0, offset_y = 0;          /* 裁剪画布左上角 → 原图 */
    int crop_w = 0, crop_h = 0;              /* 裁剪画布尺寸 */
    bool inside = false;                     /* 画布完全在图内（clamp 路径） */
    bool padded = false;                     /* 越界部分用 pad_value 填充 */
};

/* 计算 NV12/NV21 ROI 几何；失败（空 ROI / 非 YUV420SP）返回 false。 */
bool ComputeNv12CropGeometry(const AlgImage& src, const AlgBox& box, const CropConfig& cfg, CropGeometry* geom);

}  // namespace alg

#endif
