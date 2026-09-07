/**
 * @file tensor.h
 * @brief Chip-neutral tensor descriptor.
 *
 * The Tensor abstraction is the contract between layers. Pre/post-
 * processing read and write through TensorView; the backend (inferer)
 * is the only thing that knows what kind of physical memory backs the
 * `data` pointer (MMZ, DMA-buf, malloc, ...).
 */

#ifndef ALG_CORE_TENSOR_H
#define ALG_CORE_TENSOR_H

#include <array>
#include <cstdint>
#include <string>

namespace alg {

enum class DataType : int { kU8, kI8, kU16, kI16, kF16, kF32, kI32 };

enum class Layout : int { kNCHW, kNHWC, kND };

struct QuantInfo {
    float scale = 1.0f;
    int   zero_point = 0;
    bool  quantized = false;
};

struct Shape {
    static constexpr int kMaxDims = 6;
    int ndims = 0;
    std::array<int, kMaxDims> dims{};

    int Numel() const {
        int n = ndims > 0 ? 1 : 0;
        for (int i = 0; i < ndims; ++i) n *= dims[i];
        return n;
    }
};

inline int BytesPerElement(DataType dt) {
    switch (dt) {
        case DataType::kU8: case DataType::kI8: return 1;
        case DataType::kU16: case DataType::kI16: case DataType::kF16: return 2;
        case DataType::kF32: case DataType::kI32: return 4;
    }
    return 0;
}

/**
 * A view into one input or output tensor of the network.
 *
 * `data` is host-visible (the inferer takes care of cache flush /
 * invalidate around forward()). Callers may freely read/write within
 * [data, data + size_bytes).
 */
struct TensorView {
    std::string name;
    DataType    dtype = DataType::kF32;
    Layout      layout = Layout::kNCHW;
    Shape       shape;
    QuantInfo   quant;
    void*       data = nullptr;
    size_t      size_bytes = 0;
    /* 行字节跨度（行优先内存布局，仅对「按行对齐的非紧凑张量」非 0）。
     * 0 = dense：data 按 shape 紧凑连续、无行间 padding（绝大多数情况）。
     * 非 0 = 每行的实际字节跨度可能 > 行宽 × 元素大小（如 SVP ACL 输出按
     * 16 字节对齐，1x32x37 的 FLOAT 输出行跨 160B 而密集行宽 148B）。
     * 按行读取的后处理器必须用 row_stride（若为 0 则用行宽）定位各行。 */
    size_t      row_stride = 0;

    int N() const { return shape.ndims >= 4 ? shape.dims[0] : 1; }
    int C() const {
        if (layout == Layout::kNCHW && shape.ndims >= 4) return shape.dims[1];
        if (layout == Layout::kNHWC && shape.ndims >= 4) return shape.dims[3];
        if (shape.ndims >= 3) return shape.dims[0];
        return 1;
    }
    int H() const {
        if (layout == Layout::kNCHW && shape.ndims >= 4) return shape.dims[2];
        if (layout == Layout::kNHWC && shape.ndims >= 4) return shape.dims[1];
        if (shape.ndims >= 2) return shape.dims[shape.ndims - 2];
        return 0;
    }
    int W() const {
        if (layout == Layout::kNCHW && shape.ndims >= 4) return shape.dims[3];
        if (layout == Layout::kNHWC && shape.ndims >= 4) return shape.dims[2];
        if (shape.ndims >= 1) return shape.dims[shape.ndims - 1];
        return 0;
    }
};

}  // namespace alg

#endif  // ALG_CORE_TENSOR_H
