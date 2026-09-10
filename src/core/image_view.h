/**
 * @file image_view.h
 * @brief 统一解析 AlgImage 的连续内存和分平面输入。
 */

#ifndef ALG_CORE_IMAGE_VIEW_H
#define ALG_CORE_IMAGE_VIEW_H

#include <cstddef>
#include <cstdint>
#include <limits>

#include "alg_types.h"
#include "core/status.h"

namespace alg {

struct ImageView {
    const uint8_t* plane[ALG_MAX_IMAGE_PLANES] = {};
    int stride[ALG_MAX_IMAGE_PLANES] = {};
    int data_len[ALG_MAX_IMAGE_PLANES] = {};
    bool uses_separate_planes = false;
};

inline bool IsYuv420Sp(AlgPixelFormat format) {
    return format == ALG_PIX_NV12 || format == ALG_PIX_NV21;
}

inline bool CheckedPlaneByteSize(int stride, int rows, size_t* bytes) {
    if (!bytes || stride < 0 || rows < 0) return false;
    if (rows > 0 && static_cast<size_t>(stride) >
                        std::numeric_limits<size_t>::max() / static_cast<size_t>(rows)) {
        return false;
    }
    *bytes = static_cast<size_t>(stride) * static_cast<size_t>(rows);
    return true;
}

inline Status ResolveImageView(const AlgImage& image, ImageView* view) {
    if (!view || image.width <= 0 || image.height <= 0 || image.stride < 0 ||
        image.data_len < 0) {
        return ALG_E_INVALID_ARG;
    }

    int packed_row_bytes = 0;
    switch (image.format) {
        case ALG_PIX_BGR:
        case ALG_PIX_RGB:
            if (image.width > std::numeric_limits<int>::max() / 3)
                return ALG_E_INVALID_ARG;
            packed_row_bytes = image.width * 3;
            break;
        case ALG_PIX_GRAY:
            packed_row_bytes = image.width;
            break;
        case ALG_PIX_NV12:
        case ALG_PIX_NV21:
            if ((image.width & 1) || (image.height & 1)) return ALG_E_INVALID_ARG;
            packed_row_bytes = image.width;
            break;
        default:
            return ALG_E_INVALID_ARG;
    }

    *view = ImageView{};
    if (image.data) {
        const int stride = image.stride > 0 ? image.stride : packed_row_bytes;
        if (stride < packed_row_bytes) return ALG_E_INVALID_ARG;

        view->plane[0] = static_cast<const uint8_t*>(image.data);
        view->stride[0] = stride;
        view->data_len[0] = image.data_len;
        size_t y_or_packed_bytes = 0;
        if (!CheckedPlaneByteSize(stride, image.height, &y_or_packed_bytes))
            return ALG_E_INVALID_ARG;

        if (IsYuv420Sp(image.format)) {
            size_t chroma_bytes = 0;
            if (!CheckedPlaneByteSize(stride, image.height / 2, &chroma_bytes) ||
                y_or_packed_bytes > std::numeric_limits<size_t>::max() - chroma_bytes) {
                return ALG_E_INVALID_ARG;
            }
            const size_t total_bytes = y_or_packed_bytes + chroma_bytes;
            if (image.data_len > 0 && static_cast<size_t>(image.data_len) < total_bytes)
                return ALG_E_INVALID_ARG;
            view->plane[1] = view->plane[0] + y_or_packed_bytes;
            view->stride[1] = stride;
        } else if (image.data_len > 0 &&
                   static_cast<size_t>(image.data_len) < y_or_packed_bytes) {
            return ALG_E_INVALID_ARG;
        }
        return ALG_OK;
    }

    view->uses_separate_planes = true;
    if (!image.plane_data[0]) return ALG_E_INVALID_ARG;
    for (int i = 0; i < ALG_MAX_IMAGE_PLANES; ++i) {
        if (image.plane_stride[i] < 0 || image.plane_data_len[i] < 0)
            return ALG_E_INVALID_ARG;
        view->plane[i] = static_cast<const uint8_t*>(image.plane_data[i]);
        view->stride[i] = image.plane_stride[i];
        view->data_len[i] = image.plane_data_len[i];
    }

    view->stride[0] = view->stride[0] > 0
        ? view->stride[0]
        : (image.stride > 0 ? image.stride : packed_row_bytes);
    if (view->stride[0] < packed_row_bytes) return ALG_E_INVALID_ARG;
    size_t plane0_bytes = 0;
    if (!CheckedPlaneByteSize(view->stride[0], image.height, &plane0_bytes))
        return ALG_E_INVALID_ARG;
    if (view->data_len[0] > 0 &&
        static_cast<size_t>(view->data_len[0]) < plane0_bytes) {
        return ALG_E_INVALID_ARG;
    }

    if (IsYuv420Sp(image.format)) {
        if (!view->plane[1]) return ALG_E_INVALID_ARG;
        view->stride[1] = view->stride[1] > 0
            ? view->stride[1]
            : (image.stride > 0 ? image.stride : image.width);
        if (view->stride[1] < image.width) return ALG_E_INVALID_ARG;
        size_t plane1_bytes = 0;
        if (!CheckedPlaneByteSize(view->stride[1], image.height / 2, &plane1_bytes))
            return ALG_E_INVALID_ARG;
        if (view->data_len[1] > 0 &&
            static_cast<size_t>(view->data_len[1]) < plane1_bytes) {
            return ALG_E_INVALID_ARG;
        }
    }
    return ALG_OK;
}

}  // namespace alg

#endif  // ALG_CORE_IMAGE_VIEW_H
