#include "core/solution/crop_geometry.h"
#include <algorithm>
#include <cmath>

namespace alg {

/* box + crop config → 期望扩边框（可能越界；square 时为正方）。BGR 与 NV12 共用。 */
void ComputeCropRect(const AlgBox& box, const CropConfig& cfg, int* bx1, int* by1, int* bx2, int* by2) {
    float cx = (box.xmin + box.xmax) * 0.5f;
    float cy = (box.ymin + box.ymax) * 0.5f;
    float w = (box.xmax - box.xmin) * cfg.expand_ratio;
    float h = (box.ymax - box.ymin) * cfg.expand_ratio;
    if (cfg.square) {
        float side = std::max(w, h);
        w = side;
        h = side;
    }
    *bx1 = static_cast<int>(std::lround(cx - w * 0.5f));
    *by1 = static_cast<int>(std::lround(cy - h * 0.5f));
    *bx2 = static_cast<int>(std::lround(cx + w * 0.5f));
    *by2 = static_cast<int>(std::lround(cy + h * 0.5f));
    if (*bx2 <= *bx1)
        *bx2 = *bx1 + 1;
    if (*by2 <= *by1)
        *by2 = *by1 + 1;
}

namespace {

inline int EvenFloor(int v) {
    return v & ~1;
}
inline int EvenCeil(int v) {
    return (v + 1) & ~1;
}

}  // namespace

bool ComputeNv12CropGeometry(const AlgImage& src, const AlgBox& box, const CropConfig& cfg, CropGeometry* geom) {
    if (!geom || (src.format != ALG_PIX_NV12 && src.format != ALG_PIX_NV21) || src.width <= 0 || src.height <= 0 ||
        (src.width & 1) || (src.height & 1)) {
        return false;
    }
    *geom = CropGeometry{};

    const int src_w = src.width;
    const int src_h = src.height;
    int bx1, by1, bx2, by2;
    ComputeCropRect(box, cfg, &bx1, &by1, &bx2, &by2);

    const bool inside = (bx1 >= 0 && by1 >= 0 && bx2 <= src_w && by2 <= src_h);
    if (cfg.pad_value < 0 || inside) {
        /* 与 CropFromNV12 clamp 分支一致：裁到图内并做偶对齐。 */
        const int x1 = EvenFloor(std::max(0, std::min(bx1, src_w - 1)));
        const int y1 = EvenFloor(std::max(0, std::min(by1, src_h - 1)));
        int x2 = std::min(EvenCeil(std::max(x1 + 1, std::min(bx2, src_w))), src_w);
        int y2 = std::min(EvenCeil(std::max(y1 + 1, std::min(by2, src_h))), src_h);
        if (x2 - x1 < 2)
            x2 = std::min(x1 + 2, src_w);
        if (y2 - y1 < 2)
            y2 = std::min(y1 + 2, src_h);
        if (x2 - x1 < 2 || y2 - y1 < 2)
            return false;

        geom->ix1 = x1;
        geom->iy1 = y1;
        geom->ix2 = x2;
        geom->iy2 = y2;
        geom->offset_x = x1;
        geom->offset_y = y1;
        geom->crop_w = x2 - x1;
        geom->crop_h = y2 - y1;
        geom->padded = false;
        return true;
    }

    /* 与 CropFromNV12 padded 分支一致：画布越界，有效区域取图内交集。 */
    geom->offset_x = bx1;
    geom->offset_y = by1;
    geom->crop_w = bx2 - bx1;
    geom->crop_h = by2 - by1;
    if (geom->crop_w <= 0 || geom->crop_h <= 0)
        return false;

    const int ix1 = EvenCeil(std::max(0, bx1));
    const int iy1 = EvenCeil(std::max(0, by1));
    const int ix2 = EvenFloor(std::min(src_w, bx2));
    const int iy2 = EvenFloor(std::min(src_h, by2));
    if (ix2 > ix1 && iy2 > iy1) {
        geom->ix1 = ix1;
        geom->iy1 = iy1;
        geom->ix2 = ix2;
        geom->iy2 = iy2;
    } else {
        /* 框完全在画外：有效区域为空，融合路径只填 pad。 */
        geom->ix1 = geom->ix2 = 0;
        geom->iy1 = geom->iy2 = 0;
    }
    geom->padded = true;
    return true;
}

}  // namespace alg
