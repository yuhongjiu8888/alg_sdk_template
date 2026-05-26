/**
 * @file test_runner.cpp
 * @brief 通用 SDK 烟雾测试：传入 JSON solution + 一张图，跑完打印强类型结果。
 *
 * 同一个二进制可跑：
 *   ./test_runner resources/traffic_light.json   /data/test.jpg out/
 *   ./test_runner resources/speed_limit.json     /data/test.jpg out/
 *   ./test_runner resources/all.json             /data/test.jpg out/
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/time.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "alg_interface.h"

namespace {

const char* TLCName(AlgTrafficLightColor c) {
    switch (c) {
        case TLC_RED:    return "red";
        case TLC_YELLOW: return "yellow";
        case TLC_GREEN:  return "green";
        case TLC_OFF:    return "off";
        case TLC_INVALID:
        default:         return "invalid";
    }
}

int SLKmh(AlgSpeedLimitValue v) {
    /* 新枚举按 km/h÷10 连续编号（SLV_10=1 … SLV_90=9 … SLV_120=12），value×10 即 km/h。
     * 旧实现漏了 SLV_90，落到 return 0 → 90 显示成 0。 */
    if (v >= SLV_10 && v <= SLV_120) return static_cast<int>(v) * 10;
    return 0;
}

cv::Scalar TLCColor(AlgTrafficLightColor c) {
    switch (c) {
        case TLC_RED:    return cv::Scalar(  0,   0, 255);
        case TLC_YELLOW: return cv::Scalar(  0, 215, 255);
        case TLC_GREEN:  return cv::Scalar(  0, 200,   0);
        case TLC_OFF:    return cv::Scalar(160, 160, 160);
        default:         return cv::Scalar(255,   0, 255);
    }
}

void DrawBoxLabel(cv::Mat& img, const AlgBox& b, const char* text, cv::Scalar c) {
    cv::Rect rect(b.xmin, b.ymin, b.xmax - b.xmin, b.ymax - b.ymin);
    rect &= cv::Rect(0, 0, img.cols, img.rows);
    if (rect.width <= 0 || rect.height <= 0) return;
    cv::rectangle(img, rect, c, 2);
    cv::putText(img, text, cv::Point(rect.x, std::max(15, rect.y - 4)),
                cv::FONT_HERSHEY_SIMPLEX, 0.5, c, 1);
}

void DrawResult(cv::Mat& img, const AlgResult& r) {
    char buf[64];
    for (int i = 0; i < r.traffic_light_count; ++i) {
        const AlgTrafficLight& t = r.traffic_lights[i];
        std::snprintf(buf, sizeof(buf), "TL.%s %.2f", TLCName(t.color), t.box.score);
        DrawBoxLabel(img, t.box, buf, TLCColor(t.color));
    }
    for (int i = 0; i < r.speed_limit_count; ++i) {
        const AlgSpeedLimit& s = r.speed_limits[i];
        std::snprintf(buf, sizeof(buf), "SL.%d %.2f", SLKmh(s.value), s.box.score);
        DrawBoxLabel(img, s.box, buf, cv::Scalar(255, 0, 0));
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "usage: %s <solution.json> <image.jpg> [out_dir]\n", argv[0]);
        return 1;
    }
    const char* json_path  = argv[1];
    const char* image_path = argv[2];
    const char* out_dir    = argc >= 4 ? argv[3] : "out";

    cv::Mat bgr = cv::imread(image_path, cv::IMREAD_COLOR);
    if (bgr.empty()) { std::fprintf(stderr, "imread failed: %s\n", image_path); return 1; }
    std::printf("[image ] %s (%dx%d)\n", image_path, bgr.cols, bgr.rows);
    std::printf("[sdk   ] %s\n", AlgVersion());

    AlgHandle h = nullptr;
    timeval t0, t1;
    gettimeofday(&t0, nullptr);
    AlgStatus s = AlgCreate(&h, json_path);
    gettimeofday(&t1, nullptr);
    if (s != ALG_OK) { std::fprintf(stderr, "AlgCreate err %d\n", s); return s; }
    std::printf("[init  ] %.2f ms\n",
                (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_usec - t0.tv_usec) / 1000.0);

    AlgImage img;
    img.format   = ALG_PIX_BGR;
    img.width    = bgr.cols;
    img.height   = bgr.rows;
    img.stride   = static_cast<int>(bgr.step);
    img.data_len = static_cast<int>(bgr.step) * bgr.rows;
    img.data     = bgr.data;

    AlgResult r{};
    gettimeofday(&t0, nullptr);
    s = AlgRun(h, &img, &r);
    gettimeofday(&t1, nullptr);
    if (s != ALG_OK) {
        std::fprintf(stderr, "AlgRun err %d\n", s);
        AlgDestroy(h);
        return s;
    }

    std::printf("[infer ] %.2f ms\n",
                (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_usec - t0.tv_usec) / 1000.0);
    std::printf("[result] frame_id=%lld  traffic_lights=%d  speed_limits=%d\n",
                (long long)r.frame_id, r.traffic_light_count, r.speed_limit_count);
    for (int i = 0; i < r.traffic_light_count; ++i) {
        const AlgTrafficLight& t = r.traffic_lights[i];
        std::printf("  TL[%d] color=%s score=%.3f  box=(%d,%d)-(%d,%d)\n",
                    i, TLCName(t.color), t.box.score,
                    t.box.xmin, t.box.ymin, t.box.xmax, t.box.ymax);
    }
    for (int i = 0; i < r.speed_limit_count; ++i) {
        const AlgSpeedLimit& sl = r.speed_limits[i];
        std::printf("  SL[%d] %d km/h score=%.3f  box=(%d,%d)-(%d,%d)\n",
                    i, SLKmh(sl.value), sl.box.score,
                    sl.box.xmin, sl.box.ymin, sl.box.xmax, sl.box.ymax);
    }

    mkdir(out_dir, 0755);
    DrawResult(bgr, r);
    std::string out_path = std::string(out_dir) + "/result.jpg";
    if (!cv::imwrite(out_path, bgr))
        std::fprintf(stderr, "imwrite fail: %s\n", out_path.c_str());
    else
        std::printf("[save  ] %s\n", out_path.c_str());

    AlgFreeResult(&r);
    AlgDestroy(h);
    return 0;
}
