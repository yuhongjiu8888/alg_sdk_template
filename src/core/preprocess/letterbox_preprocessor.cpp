#include "core/preprocess/letterbox_preprocessor.h"

#include <algorithm>
#include <cstring>

#include <opencv2/opencv.hpp>

#include "core/logger.h"

#ifdef __aarch64__
#include <arm_neon.h>
#endif

namespace alg {

namespace {
inline int PixelStride(const AlgImage& img, int bytes_per_pixel) {
    return img.stride > 0 ? img.stride : img.width * bytes_per_pixel;
}
}  // namespace

Status LetterboxPreprocessor::Configure(const PreprocessConfig& cfg, const TensorView& input) {
    if (cfg.net_width <= 0 || cfg.net_height <= 0) return ALG_E_INVALID_ARG;
    if (cfg.layout != Layout::kNCHW) {
        ALG_LOGE("LetterboxPreprocessor currently emits NCHW only");
        return ALG_E_PREPROCESS;
    }
    if (input.dtype != DataType::kU8 && input.dtype != DataType::kF32) {
        ALG_LOGE("LetterboxPreprocessor: unsupported input dtype");
        return ALG_E_PREPROCESS;
    }
    if (input.H() != cfg.net_height || input.W() != cfg.net_width) {
        ALG_LOGE("preprocess cfg (%dx%d) != tensor shape (%dx%d)",
                 cfg.net_width, cfg.net_height, input.W(), input.H());
        return ALG_E_PREPROCESS;
    }
    cfg_ = cfg;
    net_w_ = cfg.net_width;
    net_h_ = cfg.net_height;
    /* 预计算是否需要归一化 —— 避免每帧重复判断。 */
    need_norm_ = (cfg.mean[0] != 0 || cfg.mean[1] != 0 || cfg.mean[2] != 0 ||
                  cfg.std[0] != 1 || cfg.std[1] != 1 || cfg.std[2] != 1 ||
                  cfg.scale != 1.0f);
    configured_ = true;
    return ALG_OK;
}

Status LetterboxPreprocessor::DecodeAndResize(const AlgImage& image, int new_w, int new_h) {
    const int src_h = image.height;
    const int src_w = image.width;
    const uint8_t* src = static_cast<const uint8_t*>(image.data);

    switch (image.format) {
        case ALG_PIX_NV21:
        case ALG_PIX_NV12: {
            /* NV12/NV21 路径关键优化：
             *   1) 先缩到小图再 cvtColor —— 让 colorspace 转换只处理网络输入分辨率
             *      （而不是 1080p 原图）。
             *   2) Y/UV 分别缩放后直接走 cvtColorTwoPlane，不再 copyTo 到 yuv_small_。
             *      省 2 次平面拷贝 + 这块中间缓冲的分配。
             */
            int res_h = new_h & ~1;
            int res_w = new_w & ~1;
            if (res_h < 2) res_h = 2;
            if (res_w < 2) res_w = 2;

            cv::Mat y(src_h, src_w, CV_8UC1, const_cast<uint8_t*>(src));
            cv::resize(y, y_resized_, cv::Size(res_w, res_h), 0, 0, cv::INTER_LINEAR);

            cv::Mat uv(src_h / 2, src_w / 2, CV_8UC2,
                       const_cast<uint8_t*>(src + src_h * src_w));
            cv::resize(uv, uv_resized_, cv::Size(res_w / 2, res_h / 2), 0, 0, cv::INTER_LINEAR);

            int code;
            if (image.format == ALG_PIX_NV21) {
                code = (cfg_.color == ColorOrder::kRGB) ? cv::COLOR_YUV2RGB_NV21
                     : (cfg_.color == ColorOrder::kGray) ? cv::COLOR_YUV2GRAY_NV21
                                                         : cv::COLOR_YUV2BGR_NV21;
            } else {
                code = (cfg_.color == ColorOrder::kRGB) ? cv::COLOR_YUV2RGB_NV12
                     : (cfg_.color == ColorOrder::kGray) ? cv::COLOR_YUV2GRAY_NV12
                                                         : cv::COLOR_YUV2BGR_NV12;
            }
            cv::cvtColorTwoPlane(y_resized_, uv_resized_, resized_, code);
            return ALG_OK;
        }
        case ALG_PIX_BGR: {
            const int s = PixelStride(image, 3);
            cv::Mat img(src_h, src_w, CV_8UC3, const_cast<uint8_t*>(src), s);
            cv::resize(img, resized_, cv::Size(new_w, new_h), 0, 0, cv::INTER_LINEAR);
            if (cfg_.color == ColorOrder::kRGB) cv::cvtColor(resized_, resized_, cv::COLOR_BGR2RGB);
            else if (cfg_.color == ColorOrder::kGray) cv::cvtColor(resized_, resized_, cv::COLOR_BGR2GRAY);
            return ALG_OK;
        }
        case ALG_PIX_RGB: {
            const int s = PixelStride(image, 3);
            cv::Mat img(src_h, src_w, CV_8UC3, const_cast<uint8_t*>(src), s);
            cv::resize(img, resized_, cv::Size(new_w, new_h), 0, 0, cv::INTER_LINEAR);
            if (cfg_.color == ColorOrder::kBGR) cv::cvtColor(resized_, resized_, cv::COLOR_RGB2BGR);
            else if (cfg_.color == ColorOrder::kGray) cv::cvtColor(resized_, resized_, cv::COLOR_RGB2GRAY);
            return ALG_OK;
        }
        case ALG_PIX_GRAY: {
            const int s = PixelStride(image, 1);
            cv::Mat g(src_h, src_w, CV_8UC1, const_cast<uint8_t*>(src), s);
            if (cfg_.color == ColorOrder::kGray) {
                cv::resize(g, resized_, cv::Size(new_w, new_h), 0, 0, cv::INTER_LINEAR);
            } else {
                cv::Mat tmp;
                cv::resize(g, tmp, cv::Size(new_w, new_h), 0, 0, cv::INTER_LINEAR);
                cv::cvtColor(tmp, resized_,
                             cfg_.color == ColorOrder::kRGB ? cv::COLOR_GRAY2RGB
                                                            : cv::COLOR_GRAY2BGR);
            }
            return ALG_OK;
        }
    }
    return ALG_E_INVALID_ARG;
}

void LetterboxPreprocessor::FillPaddingU8(uint8_t* plane, int new_w, int new_h,
                                          int pad_left, int pad_top) const {
    const uint8_t v = cfg_.pad_value;
    /* 上 padding：整块连续 */
    if (pad_top > 0) std::memset(plane, v, static_cast<size_t>(pad_top) * net_w_);
    /* 下 padding：整块连续 */
    const int bottom_start = pad_top + new_h;
    if (bottom_start < net_h_) {
        std::memset(plane + static_cast<size_t>(bottom_start) * net_w_, v,
                    static_cast<size_t>(net_h_ - bottom_start) * net_w_);
    }
    /* 左右 padding：仅活动区每一行的两端 */
    const int pad_right = net_w_ - pad_left - new_w;
    if (pad_left > 0 || pad_right > 0) {
        for (int y = pad_top; y < pad_top + new_h; ++y) {
            uint8_t* row = plane + static_cast<size_t>(y) * net_w_;
            if (pad_left > 0)  std::memset(row, v, pad_left);
            if (pad_right > 0) std::memset(row + pad_left + new_w, v, pad_right);
        }
    }
}

void LetterboxPreprocessor::FillPaddingF32(float* plane, int new_w, int new_h,
                                           int pad_left, int pad_top) const {
    const float v = static_cast<float>(cfg_.pad_value);
    /* float 路径在端侧极少用，简单实现即可。 */
    if (pad_top > 0) std::fill_n(plane, static_cast<size_t>(pad_top) * net_w_, v);
    const int bottom_start = pad_top + new_h;
    if (bottom_start < net_h_)
        std::fill_n(plane + static_cast<size_t>(bottom_start) * net_w_,
                    static_cast<size_t>(net_h_ - bottom_start) * net_w_, v);
    const int pad_right = net_w_ - pad_left - new_w;
    if (pad_left > 0 || pad_right > 0) {
        for (int y = pad_top; y < pad_top + new_h; ++y) {
            float* row = plane + static_cast<size_t>(y) * net_w_;
            if (pad_left > 0)  std::fill_n(row, pad_left, v);
            if (pad_right > 0) std::fill_n(row + pad_left + new_w, pad_right, v);
        }
    }
}

Status LetterboxPreprocessor::WriteToTensor(const TensorView& input,
                                            int new_w, int new_h,
                                            int pad_left, int pad_top) {
    const int channels = (cfg_.color == ColorOrder::kGray) ? 1 : 3;
    const int HW = net_h_ * net_w_;

    if (input.dtype == DataType::kU8 && !need_norm_) {
        uint8_t* base = static_cast<uint8_t*>(input.data);

        /* 仅当确实有 padding 时才 memset；stretch 模式直接跳过。 */
        const bool any_pad = (pad_left || pad_top ||
                              new_w != net_w_ || new_h != net_h_);

        if (channels == 3) {
            uint8_t* p0 = base + 0 * HW;
            uint8_t* p1 = base + 1 * HW;
            uint8_t* p2 = base + 2 * HW;
            if (any_pad) {
                FillPaddingU8(p0, new_w, new_h, pad_left, pad_top);
                FillPaddingU8(p1, new_w, new_h, pad_left, pad_top);
                FillPaddingU8(p2, new_w, new_h, pad_left, pad_top);
            }
            for (int y = 0; y < new_h; ++y) {
                const uint8_t* row = resized_.ptr<uint8_t>(y);
                const int dst_off = (y + pad_top) * net_w_ + pad_left;
                int x = 0;
#ifdef __aarch64__
                for (; x + 15 < new_w; x += 16) {
                    uint8x16x3_t v = vld3q_u8(row + x * 3);
                    vst1q_u8(p0 + dst_off + x, v.val[0]);
                    vst1q_u8(p1 + dst_off + x, v.val[1]);
                    vst1q_u8(p2 + dst_off + x, v.val[2]);
                }
#endif
                for (; x < new_w; ++x) {
                    const int si = x * 3;
                    p0[dst_off + x] = row[si];
                    p1[dst_off + x] = row[si + 1];
                    p2[dst_off + x] = row[si + 2];
                }
            }
        } else {
            if (any_pad) FillPaddingU8(base, new_w, new_h, pad_left, pad_top);
            for (int y = 0; y < new_h; ++y) {
                const uint8_t* row = resized_.ptr<uint8_t>(y);
                uint8_t* dst = base + (y + pad_top) * net_w_ + pad_left;
                std::memcpy(dst, row, new_w);
            }
        }
        return ALG_OK;
    }

    if (input.dtype == DataType::kF32) {
        float* base = static_cast<float*>(input.data);
        const bool any_pad = (pad_left || pad_top ||
                              new_w != net_w_ || new_h != net_h_);

        const float inv_std[3] = {cfg_.scale / cfg_.std[0],
                                  cfg_.scale / (channels > 1 ? cfg_.std[1] : 1.0f),
                                  cfg_.scale / (channels > 2 ? cfg_.std[2] : 1.0f)};
        for (int c = 0; c < channels; ++c) {
            if (any_pad) FillPaddingF32(base + c * HW, new_w, new_h, pad_left, pad_top);
        }
        for (int y = 0; y < new_h; ++y) {
            const uint8_t* row = resized_.ptr<uint8_t>(y);
            const int dst_off = (y + pad_top) * net_w_ + pad_left;
            for (int x = 0; x < new_w; ++x) {
                const int si = x * channels;
                for (int c = 0; c < channels; ++c) {
                    float v = (static_cast<float>(row[si + c]) - cfg_.mean[c]) * inv_std[c];
                    base[c * HW + dst_off + x] = v;
                }
            }
        }
        return ALG_OK;
    }

    ALG_LOGE("unsupported tensor dtype for preprocess output");
    return ALG_E_PREPROCESS;
}

Status LetterboxPreprocessor::Apply(const AlgImage& image, TensorView& input,
                                    PreprocessState& state) {
    if (!configured_) return ALG_E_NOT_INITIALIZED;
    if (image.width <= 0 || image.height <= 0) return ALG_E_INVALID_ARG;

    float scale;
    int new_w, new_h, pad_left = 0, pad_top = 0;
    switch (cfg_.resize) {
        case ResizeMode::kStretch:
            scale = 1.0f;
            new_w = net_w_;
            new_h = net_h_;
            break;
        case ResizeMode::kLetterboxTL:
            scale = static_cast<float>(net_w_) / std::max(image.width, image.height);
            new_w = static_cast<int>(image.width * scale);
            new_h = static_cast<int>(image.height * scale);
            break;
        case ResizeMode::kLetterboxCenter:
            scale = std::min(static_cast<float>(net_w_) / image.width,
                             static_cast<float>(net_h_) / image.height);
            new_w = static_cast<int>(image.width * scale);
            new_h = static_cast<int>(image.height * scale);
            pad_left = (net_w_ - new_w) / 2;
            pad_top = (net_h_ - new_h) / 2;
            break;
    }
    if (new_w <= 0 || new_h <= 0) return ALG_E_INVALID_ARG;

    Status s = DecodeAndResize(image, new_w, new_h);
    if (s != ALG_OK) return s;

    new_w = resized_.cols;
    new_h = resized_.rows;

    s = WriteToTensor(input, new_w, new_h, pad_left, pad_top);
    if (s != ALG_OK) return s;

    state.scale_ratio = scale;
    state.pad_left = pad_left;
    state.pad_top = pad_top;
    state.original_width = image.width;
    state.original_height = image.height;
    return ALG_OK;
}

}  // namespace alg
