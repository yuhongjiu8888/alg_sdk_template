/** Raw NV21 smoke/loop runner. Uses only the public C API and the C++ runtime. */
#include "alg_interface.h"
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <limits>
#include <vector>

namespace {
const char* SignName(AlgSignType type) {
    switch (type) {
        case SIGN_NO_PARKING:
            return "no_parking";
        case SIGN_PARE:
            return "pare";
        case SIGN_INVALID:
        default:
            return "invalid";
    }
}

bool ParsePositive(const char* value, int* out) {
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(value, &end, 10);
    if (errno || end == value || *end || parsed <= 0 || parsed > INT_MAX)
        return false;
    *out = static_cast<int>(parsed);
    return true;
}

double Milliseconds(const timespec& begin, const timespec& end) {
    return (end.tv_sec - begin.tv_sec) * 1000.0 + (end.tv_nsec - begin.tv_nsec) / 1e6;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 5 || argc > 7) {
        std::fprintf(stderr, "Usage: %s config.json frame.nv21 width height [stride] [iterations]\n", argv[0]);
        return 2;
    }
    int width = 0, height = 0, stride = 0, iterations = 1;
    if (!ParsePositive(argv[3], &width) || !ParsePositive(argv[4], &height) || (width & 1) || (height & 1) ||
        (argc >= 6 && !ParsePositive(argv[5], &stride)) ||
        (argc >= 7 && !ParsePositive(argv[6], &iterations))) {
        std::fprintf(stderr, "Invalid dimensions, stride or iterations; NV21 width/height must be even.\n");
        return 2;
    }
    if (!stride)
        stride = width;
    // AlgImage::data_len is int; check before calculating or allocating the frame.
    const size_t rows = static_cast<size_t>(height) + height / 2;
    if (stride < width || rows > static_cast<size_t>(INT_MAX) / static_cast<size_t>(stride)) {
        std::fprintf(stderr, "Frame exceeds AlgImage capacity or stride is smaller than width.\n");
        return 2;
    }
    const size_t bytes = static_cast<size_t>(stride) * rows;
    std::vector<unsigned char> frame;
    try {
        frame.resize(bytes);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Cannot allocate frame: %s\n", e.what());
        return 1;
    }
    FILE* file = std::fopen(argv[2], "rb");
    if (!file) {
        std::perror(argv[2]);
        return 1;
    }
    const size_t read = std::fread(frame.data(), 1, bytes, file);
    const bool read_error = std::ferror(file) != 0;
    const int trailing = std::fgetc(file);
    std::fclose(file);
    if (read_error || read != bytes || trailing != EOF) {
        std::fprintf(stderr, "Expected exactly %zu bytes of one stride-padded NV21 frame.\n", bytes);
        return 1;
    }

    AlgSetLogLevel(ALG_LOG_INFO);
    AlgHandle handle = nullptr;
    AlgStatus status = AlgCreate(&handle, argv[1]);
    if (status != ALG_OK) {
        std::fprintf(stderr, "AlgCreate failed: %d\n", status);
        return 1;
    }
    AlgImage image{};
    image.format = ALG_PIX_NV21;
    image.width = width;
    image.height = height;
    image.stride = stride;
    image.data = frame.data();
    image.data_len = static_cast<int>(bytes);
    timespec begin{}, end{}, cpu_begin{}, cpu_end{};
    clock_gettime(CLOCK_MONOTONIC, &begin);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_begin);
    for (int i = 0; i < iterations; ++i) {
        AlgResult result{};
        status = AlgRun(handle, &image, &result);
        if (status != ALG_OK) {
            std::fprintf(stderr, "AlgRun iteration %d failed: %d\n", i, status);
            break;
        }
        std::printf("frame=%lld speed_limits=%d signs=%d license_plates=%d\n", result.frame_id,
                    result.speed_limit_count, result.sign_count, result.license_plate_count);
        for (int j = 0; j < result.speed_limit_count; ++j)
            std::printf("  speed=%d score=%.4f\n", static_cast<int>(result.speed_limits[j].value) * 10,
                        result.speed_limits[j].box.score);
        for (int j = 0; j < result.sign_count; ++j) {
            const AlgSign& sign = result.signs[j];
            std::printf("  sign=%s score=%.4f box=(%d,%d)-(%d,%d)\n", SignName(sign.type), sign.box.score,
                        sign.box.xmin, sign.box.ymin, sign.box.xmax, sign.box.ymax);
        }
        for (int j = 0; j < result.license_plate_count; ++j)
            std::printf("  plate=%s score=%.4f\n", result.license_plates[j].text,
                        result.license_plates[j].box.score);
    }
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_end);
    clock_gettime(CLOCK_MONOTONIC, &end);
    std::printf("backend=%s wall=%.2fms thread_cpu=%.2fms requested_iterations=%d\n", AlgBackendName(),
                Milliseconds(begin, end), Milliseconds(cpu_begin, cpu_end), iterations);
    AlgDestroy(handle);
    return status == ALG_OK ? 0 : 1;
}
