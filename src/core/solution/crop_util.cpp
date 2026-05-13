#include "core/solution/crop_util.h"

#include <algorithm>

#include <opencv2/opencv.hpp>

#include "core/logger.h"

namespace alg {

void DecodeSourceToBgrInto(const AlgImage& src, cv::Mat* out) {
    if (!out) return;
    const uint8_t* p = static_cast<const uint8_t*>(src.data);
    const int stride = src.stride > 0 ? src.stride : 0;

    switch (src.format) {
        case ALG_PIX_BGR:
            /* 零拷贝包装：直接把 out 重定向到用户 buffer，共享内存。 */
            *out = cv::Mat(src.height, src.width, CV_8UC3,
                           const_cast<uint8_t*>(p),
                           stride ? stride : src.width * 3);
            return;
        case ALG_PIX_RGB: {
            cv::Mat m(src.height, src.width, CV_8UC3,
                      const_cast<uint8_t*>(p),
                      stride ? stride : src.width * 3);
            cv::cvtColor(m, *out, cv::COLOR_RGB2BGR);  /* 复用 *out 的 buffer */
            return;
        }
        case ALG_PIX_GRAY: {
            cv::Mat m(src.height, src.width, CV_8UC1,
                      const_cast<uint8_t*>(p),
                      stride ? stride : src.width);
            cv::cvtColor(m, *out, cv::COLOR_GRAY2BGR);
            return;
        }
        case ALG_PIX_NV12:
        case ALG_PIX_NV21: {
            cv::Mat yuv(src.height * 3 / 2, src.width, CV_8UC1,
                        const_cast<uint8_t*>(p));
            cv::cvtColor(yuv, *out,
                         src.format == ALG_PIX_NV12 ? cv::COLOR_YUV2BGR_NV12
                                                    : cv::COLOR_YUV2BGR_NV21);
            return;
        }
    }
    out->release();
}

bool CropFromDecoded(const cv::Mat& decoded_bgr, const AlgBox& box,
                     const CropConfig& cfg,
                     cv::Mat* holder, AlgImage* out, CropTransform* xf) {
    if (!holder || !out || !xf) return false;
    if (decoded_bgr.empty()) return false;

    const int src_w = decoded_bgr.cols;
    const int src_h = decoded_bgr.rows;

    float cx = (box.xmin + box.xmax) * 0.5f;
    float cy = (box.ymin + box.ymax) * 0.5f;
    float w  = (box.xmax - box.xmin) * cfg.expand_ratio;
    float h  = (box.ymax - box.ymin) * cfg.expand_ratio;
    if (cfg.square) {
        float side = std::max(w, h);
        w = side; h = side;
    }
    int x1 = static_cast<int>(cx - w * 0.5f);
    int y1 = static_cast<int>(cy - h * 0.5f);
    int x2 = static_cast<int>(cx + w * 0.5f);
    int y2 = static_cast<int>(cy + h * 0.5f);
    x1 = std::max(0, std::min(x1, src_w - 1));
    y1 = std::max(0, std::min(y1, src_h - 1));
    x2 = std::max(x1 + 1, std::min(x2, src_w));
    y2 = std::max(y1 + 1, std::min(y2, src_h));

    /* 关键优化：ROI 视图，零拷贝。共享 decoded_bgr 的像素。 */
    *holder = decoded_bgr(cv::Rect(x1, y1, x2 - x1, y2 - y1));

    out->format   = ALG_PIX_BGR;
    out->width    = holder->cols;
    out->height   = holder->rows;
    out->stride   = static_cast<int>(holder->step);  /* 非连续，下游必须按 stride 读 */
    out->data_len = static_cast<int>(holder->step) * holder->rows;
    out->data     = holder->data;

    xf->offset_x = x1;
    xf->offset_y = y1;
    xf->crop_w   = holder->cols;
    xf->crop_h   = holder->rows;
    return true;
}

}  // namespace alg
