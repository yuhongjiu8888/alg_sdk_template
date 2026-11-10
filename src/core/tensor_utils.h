/** Tensor element decoding shared by model postprocessors. */
#ifndef ALG_CORE_TENSOR_UTILS_H
#define ALG_CORE_TENSOR_UTILS_H

#include <cstddef>
#include <cstring>
#include "core/tensor.h"

namespace alg {

inline float HalfToFloat(uint16_t h) {
    uint32_t sign = (h & 0x8000u) << 16;
    uint32_t exp = (h & 0x7C00u) >> 10;
    uint32_t mant = h & 0x03FFu;
    uint32_t f;
    if (exp == 0) {
        if (mant == 0) {
            f = sign;
        } else {
            exp = 1;
            while ((mant & 0x0400u) == 0) {
                mant <<= 1;
                exp--;
            }
            mant &= 0x03FFu;
            f = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
        }
    } else if (exp == 0x1F) {
        f = sign | 0x7F800000u | (mant << 13);
    } else {
        f = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
    }
    float out;
    std::memcpy(&out, &f, sizeof(out));
    return out;
}

/* p 指向一个可读元素，允许非对齐地址；调用方负责按 row_stride 定位。
 * 整型按 (raw - zero_point) * scale 反量化，浮点直接解码。
 * I32 保留原字节读取路径先转 float 再相减的舍入顺序。 */
inline float ReadElemAt(const TensorView& t, const uint8_t* p) {
    switch (t.dtype) {
        case DataType::kF32: {
            float v;
            std::memcpy(&v, p, sizeof(v));
            return v;
        }
        case DataType::kI8: {
            int8_t raw;
            std::memcpy(&raw, p, sizeof(raw));
            return (static_cast<int>(raw) - t.quant.zero_point) * t.quant.scale;
        }
        case DataType::kU8: {
            uint8_t raw;
            std::memcpy(&raw, p, sizeof(raw));
            return (static_cast<int>(raw) - t.quant.zero_point) * t.quant.scale;
        }
        case DataType::kI16: {
            int16_t raw;
            std::memcpy(&raw, p, sizeof(raw));
            return (static_cast<int>(raw) - t.quant.zero_point) * t.quant.scale;
        }
        case DataType::kU16: {
            uint16_t raw;
            std::memcpy(&raw, p, sizeof(raw));
            return (static_cast<int>(raw) - t.quant.zero_point) * t.quant.scale;
        }
        case DataType::kI32: {
            int32_t raw;
            std::memcpy(&raw, p, sizeof(raw));
            return (static_cast<float>(raw) - static_cast<float>(t.quant.zero_point)) * t.quant.scale;
        }
        case DataType::kF16: {
            uint16_t h;
            std::memcpy(&h, p, sizeof(h));
            return HalfToFloat(h);
        }
    }
    return 0.0f;
}

/* idx 是物理缓冲内的元素下标，不自动跳过行 padding；调用方保证下标有效
 * 且 data 按 dtype 对齐。保留直接类型读取，避免检测热点路径按字节装载。
 * I32 保留原下标读取路径先做整数减法再转 float 的顺序。 */
inline float ReadElem(const TensorView& t, size_t idx) {
    switch (t.dtype) {
        case DataType::kF32:
            return static_cast<const float*>(t.data)[idx];
        case DataType::kI8: {
            int raw = static_cast<const int8_t*>(t.data)[idx];
            return (raw - t.quant.zero_point) * t.quant.scale;
        }
        case DataType::kU8: {
            int raw = static_cast<const uint8_t*>(t.data)[idx];
            return (raw - t.quant.zero_point) * t.quant.scale;
        }
        case DataType::kI16: {
            int raw = static_cast<const int16_t*>(t.data)[idx];
            return (raw - t.quant.zero_point) * t.quant.scale;
        }
        case DataType::kU16: {
            int raw = static_cast<const uint16_t*>(t.data)[idx];
            return (raw - t.quant.zero_point) * t.quant.scale;
        }
        case DataType::kI32: {
            int raw = static_cast<const int32_t*>(t.data)[idx];
            return (raw - t.quant.zero_point) * t.quant.scale;
        }
        case DataType::kF16:
            return HalfToFloat(static_cast<const uint16_t*>(t.data)[idx]);
    }
    return 0.0f;
}

}  // namespace alg

#endif  // ALG_CORE_TENSOR_UTILS_H
