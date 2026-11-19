#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include "core/postprocess/box_transform.h"
#include "core/postprocess/math_utils.h"

namespace {

void Check(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

template <typename T>
void CheckQuantized(alg::DataType dtype, T low, T high) {
    T values[] = {low, 0, high};
    alg::TensorView t;
    t.dtype = dtype;
    t.data = values;
    t.quant.scale = 0.25f;
    t.quant.zero_point = 3;
    for (size_t i = 0; i < 3; ++i) {
        const float expected = (static_cast<int>(values[i]) - 3) * 0.25f;
        Check(alg::ReadElem(t, i) == expected, "quantized indexed read");
        // Deliberately unaligned byte address, as permitted by ReadElemAt.
        uint8_t bytes[sizeof(T) + 1];
        std::memcpy(bytes + 1, values + i, sizeof(T));
        Check(alg::ReadElemAt(t, bytes + 1) == expected, "quantized byte read");
    }
}

void TestTensorReads() {
    CheckQuantized<int8_t>(alg::DataType::kI8, -128, 127);
    CheckQuantized<uint8_t>(alg::DataType::kU8, 0, 255);
    CheckQuantized<int16_t>(alg::DataType::kI16, -32768, 32767);
    CheckQuantized<uint16_t>(alg::DataType::kU16, 0, 65535);
    CheckQuantized<int32_t>(alg::DataType::kI32, -100000, 100000);

    alg::TensorView t;
    int32_t raw = 16777217;
    t.dtype = alg::DataType::kI32;
    t.data = &raw;
    t.quant.zero_point = 16777216;
    Check(alg::ReadElem(t, 0) == 1.0f, "indexed I32 subtracts before float conversion");
    Check(alg::ReadElemAt(t, reinterpret_cast<const uint8_t*>(&raw)) == 0.0f,
          "byte I32 preserves float conversion before subtraction");

    float values[] = {-3.5f, 0.0f, 9.25f};
    t.dtype = alg::DataType::kF32;
    t.data = values;
    t.quant.scale = 100.0f;
    for (size_t i = 0; i < 3; ++i) {
        Check(alg::ReadElem(t, i) == values[i], "F32 bypasses quantization");
        uint8_t bytes[sizeof(float) + 1];
        std::memcpy(bytes + 1, values + i, sizeof(float));
        Check(alg::ReadElemAt(t, bytes + 1) == values[i], "unaligned F32 read");
    }

    // Two rows with padding and a nonzero starting byte offset.
    uint8_t rows[17] = {};
    float a = 3.0f, b = 7.0f;
    std::memcpy(rows + 1, &a, sizeof(a));
    std::memcpy(rows + 9, &b, sizeof(b));
    t.data = rows + 1;
    t.row_stride = 8;
    Check(alg::ReadElemAt(t, static_cast<const uint8_t*>(t.data)) == a, "padded first row");
    Check(alg::ReadElemAt(t, static_cast<const uint8_t*>(t.data) + t.row_stride) == b, "padded second row");
}

void TestHalf() {
    // Independent arithmetic reference for every finite binary16 encoding.
    for (uint32_t bits = 0; bits <= 0xFFFFu; ++bits) {
        uint16_t h = static_cast<uint16_t>(bits);
        const int exponent = (h >> 10) & 31;
        const int fraction = h & 1023;
        const bool negative = (h & 0x8000u) != 0;
        const float actual = alg::HalfToFloat(h);
        if (exponent == 31) {
            Check(fraction ? std::isnan(actual) : std::isinf(actual), "F16 NaN/infinity");
        } else {
            float expected = exponent ? std::ldexp(static_cast<float>(1024 + fraction), exponent - 25)
                                      : std::ldexp(static_cast<float>(fraction), -24);
            if (negative)
                expected = -expected;
            Check(actual == expected, "F16 finite conversion");
        }
        Check(static_cast<bool>(std::signbit(actual)) == negative, "F16 sign including signed zero");
        alg::TensorView t;
        t.dtype = alg::DataType::kF16;
        t.data = &h;
        const float indexed = alg::ReadElem(t, 0);
        uint8_t bytes[3];
        std::memcpy(bytes + 1, &h, sizeof(h));
        const float unaligned = alg::ReadElemAt(t, bytes + 1);
        Check((std::isnan(actual) && std::isnan(indexed) && std::isnan(unaligned)) ||
                  (actual == indexed && actual == unaligned),
              "F16 indexed and byte paths");
    }
}

void TestMath() {
    Check(alg::Sigmoid(0.0f) == 0.5f, "sigmoid midpoint");
    Check(alg::Sigmoid(-1000.0f) == 0.0f && alg::Sigmoid(1000.0f) == 1.0f, "sigmoid extremes");
    const float inf = std::numeric_limits<float>::infinity();
    Check(alg::LogitThreshold(0.0f) == -inf && alg::LogitThreshold(-1.0f) == -inf, "lower threshold bound");
    Check(alg::LogitThreshold(1.0f) == inf && alg::LogitThreshold(2.0f) == inf, "upper threshold bound");
    Check(alg::LogitThreshold(0.5f) == -1e-6f, "threshold rounding tolerance");
    for (float p : {0.01f, 0.25f, 0.7f, 0.99f}) {
        Check(alg::LogitThreshold(p) < std::log(p / (1.0f - p)), "threshold is biased toward lower logits");
        Check(std::fabs(alg::Sigmoid(alg::LogitThreshold(p)) - p) < 1e-6f, "threshold probability round trip");
    }

    float values[] = {9999.0f, 1000.0f, 1001.0f, 1001.0f, -9999.0f};
    alg::TensorView t;
    t.data = values;
    int arg;
    float prob;
    alg::SoftmaxArgmax(t, 1, 3, &arg, &prob);
    Check(arg == 1, "softmax base offset and first maximum on ties");
    Check(std::fabs(prob - 1.0f / (2.0f + std::exp(-1.0f))) < 1e-7f, "stable softmax at large logits");
    alg::SoftmaxArgmax(t, 4, 1, &arg, &prob);
    Check(arg == 0 && prob == 1.0f, "single class");

    int8_t quantized[] = {-10, 0, 2};
    t.dtype = alg::DataType::kI8;
    t.data = quantized;
    t.quant.scale = 0.5f;
    t.quant.zero_point = 2;
    alg::SoftmaxArgmax(t, 1, 2, &arg, &prob);
    Check(arg == 1 && std::fabs(prob - alg::Sigmoid(1.0f)) < 1e-7f, "softmax dequantizes logits");
}

void TestBoxTransform() {
    alg::PreprocessState state;
    state.original_width = 100;
    state.original_height = 80;
    state.scale_ratio = 0.5f;
    state.pad_left = 10;
    state.pad_top = 20;
    alg::Proposal p;
    p.x1 = 11.25f;
    p.y1 = 22.25f;
    p.x2 = 1000.0f;
    p.y2 = 1000.0f;
    p.score = 0.75f;
    AlgBox box = alg::DetectionBoxTransform(state).Apply(p);
    Check(box.xmin == 3 && box.ymin == 5 && box.xmax == 99 && box.ymax == 79 && box.score == p.score,
          "letterbox inverse, rounding, upper clipping and score");

    state.scale_x = 0.25f;
    state.scale_y = 1.0f;
    box = alg::DetectionBoxTransform(state).Apply(p);
    Check(box.xmin == 5 && box.ymin == 2, "per-axis scaling overrides common ratio");
    state.scale_x = 0.0f;
    box = alg::DetectionBoxTransform(state).Apply(p);
    Check(box.xmin == 3 && box.ymin == 2, "independent per-axis fallback");
    state.scale_ratio = 0.0f;
    state.scale_y = 0.0f;
    box = alg::DetectionBoxTransform(state).Apply(p);
    Check(box.xmin == 1 && box.ymin == 2, "invalid scale falls back to identity");
    p.x1 = -100.0f;
    p.y1 = -100.0f;
    box = alg::DetectionBoxTransform(state).Apply(p);
    Check(box.xmin == 0 && box.ymin == 0, "lower clipping");
}

}  // namespace

int main() {
    TestTensorReads();
    TestHalf();
    TestMath();
    TestBoxTransform();
    std::puts("postprocess utility tests passed");
}
