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
