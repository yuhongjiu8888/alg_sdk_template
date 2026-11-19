#include "core/solution/crop_util.h"
#include <algorithm>
#include <opencv2/opencv.hpp>
#include "core/image_view.h"
#include "core/logger.h"

namespace alg {

void DecodeSourceToBgrInto(const AlgImage& src, cv::Mat* out) {
    if (!out)
        return;
    ImageView source;
    if (ResolveImageView(src, &source) != ALG_OK) {
        out->release();
        return;
    }
    const uint8_t* p = source.plane[0];
    const int stride = source.stride[0];

    switch (src.format) {
        case ALG_PIX_BGR:
            /* 零拷贝包装：直接把 out 重定向到用户 buffer，共享内存。 */
            *out = cv::Mat(src.height, src.width, CV_8UC3, const_cast<uint8_t*>(p), stride);
            return;
        case ALG_PIX_RGB: {
            cv::Mat m(src.height, src.width, CV_8UC3, const_cast<uint8_t*>(p), stride);
            cv::cvtColor(m, *out, cv::COLOR_RGB2BGR); /* 复用 *out 的 buffer */
            return;
        }
        case ALG_PIX_GRAY: {
            cv::Mat m(src.height, src.width, CV_8UC1, const_cast<uint8_t*>(p), stride);
            cv::cvtColor(m, *out, cv::COLOR_GRAY2BGR);
            return;
        }
        case ALG_PIX_NV12:
        case ALG_PIX_NV21: {
            cv::Mat y(src.height, src.width, CV_8UC1, const_cast<uint8_t*>(p), source.stride[0]);
            cv::Mat uv(src.height / 2, src.width / 2, CV_8UC2, const_cast<uint8_t*>(source.plane[1]), source.stride[1]);
            cv::cvtColorTwoPlane(y, uv, *out,
                                 src.format == ALG_PIX_NV12 ? cv::COLOR_YUV2BGR_NV12 : cv::COLOR_YUV2BGR_NV21);
            return;
        }
    }
    out->release();
}

namespace {

/* 从 NV12/NV21 原图抠 ROI 子图：Y/UV 平面各出一个带源 stride 的 Mat 视图，
 * 只对小区域 cvtColorTwoPlane → BGR，避免整帧 NV12→BGR。 */
void ConvertNvSubRegions(const uint8_t* y_plane,
                         const uint8_t* uv_plane,
                         int src_stride_y,
                         int src_stride_uv,
                         int x1,
                         int y1,
                         int x2,
                         int y2,
                         bool nv21,
                         cv::Mat* bgr) {
    cv::Mat y_sub(y2 - y1, x2 - x1, CV_8UC1,
                  const_cast<uint8_t*>(y_plane + static_cast<size_t>(y1) * src_stride_y + x1), src_stride_y);
    cv::Mat uv_sub(
        (y2 - y1) / 2, (x2 - x1) / 2, CV_8UC2,
        const_cast<uint8_t*>(uv_plane + static_cast<size_t>(y1 / 2) * src_stride_uv + static_cast<size_t>(x1 / 2) * 2),
        src_stride_uv);
    cv::cvtColorTwoPlane(y_sub, uv_sub, *bgr, nv21 ? cv::COLOR_YUV2BGR_NV21 : cv::COLOR_YUV2BGR_NV12);
}

}  // namespace

bool CropFromDecoded(const cv::Mat& decoded_bgr,
                     const AlgBox& box,
                     const CropConfig& cfg,
                     cv::Mat* holder,
                     AlgImage* out,
                     CropTransform* xf) {
    if (!holder || !out || !xf)
        return false;
    if (decoded_bgr.empty())
        return false;

    const int src_w = decoded_bgr.cols;
    const int src_h = decoded_bgr.rows;

    int bx1, by1, bx2, by2;
    ComputeCropRect(box, cfg, &bx1, &by1, &bx2, &by2);

    const bool inside = (bx1 >= 0 && by1 >= 0 && bx2 <= src_w && by2 <= src_h);

    if (cfg.pad_value < 0 || inside) {
        /* 旧行为：裁到图内（零拷贝 ROI 视图）。inside 时与扩边框一致；越界时 clamp
         * 到边界（可能裁成非正方，下游 resize 会形变）。 */
        int x1 = std::max(0, std::min(bx1, src_w - 1));
        int y1 = std::max(0, std::min(by1, src_h - 1));
        int x2 = std::max(x1 + 1, std::min(bx2, src_w));
        int y2 = std::max(y1 + 1, std::min(by2, src_h));
        *holder = decoded_bgr(cv::Rect(x1, y1, x2 - x1, y2 - y1));
        out->stride = static_cast<int>(holder->step); /* 非连续，下游必须按 stride 读 */
        xf->offset_x = x1;
        xf->offset_y = y1;
    } else {
        /* padded 模式：产出完整扩边（square 时为正方）区域，越界部分用 pad_value 灰填，
         * 与训练端 infer.py::_crop_for_classifier 的 cv2.warpAffine(borderValue=114) 等价。
         * 否则贴边的牌被 clamp+stretch 形变，OCR 逐位读数会把最左/最右位读错（如 110→10）。 */
        const int cw = bx2 - bx1;
        const int ch = by2 - by1;
        holder->create(ch, cw, CV_8UC3);
        holder->setTo(cv::Scalar::all(cfg.pad_value));
        const int ix1 = std::max(0, bx1), iy1 = std::max(0, by1);
        const int ix2 = std::min(src_w, bx2), iy2 = std::min(src_h, by2);
        if (ix2 > ix1 && iy2 > iy1) {
            decoded_bgr(cv::Rect(ix1, iy1, ix2 - ix1, iy2 - iy1))
                .copyTo((*holder)(cv::Rect(ix1 - bx1, iy1 - by1, ix2 - ix1, iy2 - iy1)));
        }
        out->stride = static_cast<int>(holder->step);
        xf->offset_x = bx1; /* 可能为负：MapBackToOriginal 加回后仍是正确原图坐标 */
        xf->offset_y = by1;
    }

    out->format = ALG_PIX_BGR;
    out->width = holder->cols;
    out->height = holder->rows;
    out->data_len = static_cast<int>(holder->step) * holder->rows;
    out->data = holder->data;
    return true;
}

bool CropFromNV12(
    const AlgImage& src, const AlgBox& box, const CropConfig& cfg, cv::Mat* holder, AlgImage* out, CropTransform* xf) {
    if (!holder || !out || !xf)
        return false;
    if (src.format != ALG_PIX_NV12 && src.format != ALG_PIX_NV21)
        return false;
    ImageView source;
    if (ResolveImageView(src, &source) != ALG_OK)
        return false;

    CropGeometry geom;
    if (!ComputeNv12CropGeometry(src, box, cfg, &geom))
        return false;

    if (!geom.padded) {
        ConvertNvSubRegions(source.plane[0], source.plane[1], source.stride[0], source.stride[1],
                            geom.ix1, geom.iy1, geom.ix2, geom.iy2, src.format == ALG_PIX_NV21, holder);
    } else {
        holder->create(geom.crop_h, geom.crop_w, CV_8UC3);
        holder->setTo(cv::Scalar::all(cfg.pad_value));
        if (geom.ix2 > geom.ix1 && geom.iy2 > geom.iy1) {
            const cv::Rect roi(geom.ix1 - geom.offset_x, geom.iy1 - geom.offset_y,
                               geom.ix2 - geom.ix1, geom.iy2 - geom.iy1);
            cv::Mat dst_roi = (*holder)(roi);
            const uint8_t* expected = holder->ptr<uint8_t>(roi.y) + static_cast<size_t>(roi.x) * 3;
            ConvertNvSubRegions(source.plane[0], source.plane[1], source.stride[0], source.stride[1],
                                geom.ix1, geom.iy1, geom.ix2, geom.iy2, src.format == ALG_PIX_NV21, &dst_roi);
            if (dst_roi.data != expected)
                dst_roi.copyTo((*holder)(roi));
        }
    }

    out->format = ALG_PIX_BGR;
    out->width = holder->cols;
    out->height = holder->rows;
    out->stride = static_cast<int>(holder->step);
    out->data = holder->data;
    out->data_len = static_cast<int>(holder->step) * holder->rows;
    xf->offset_x = geom.offset_x;
    xf->offset_y = geom.offset_y;
    return true;
}

}  // namespace alg
