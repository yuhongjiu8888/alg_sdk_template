/**
 * @file test_runner.cpp
 * @brief 通用 SDK 烟雾测试：传入 JSON solution 配置 + 一张图，跑完打印结果。
 *
 * 同一个二进制可跑：
 *   ./test_runner resources/traffic_light.json   /data/test.jpg out/
 *   ./test_runner resources/speed_limit.json     /data/test.jpg out/
 *
 * 业务编排细节都在 JSON 里，不需要写新代码。
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "alg_interface.h"

namespace {

void DrawResult(cv::Mat& img, const AlgResult& r) {
    static const cv::Scalar kColors[] = {
        cv::Scalar(  0,   0, 255), cv::Scalar(  0, 215, 255),
        cv::Scalar(  0, 200,   0), cv::Scalar(160, 160, 160),
        cv::Scalar(255,   0,   0), cv::Scalar(255,   0, 255),
        cv::Scalar(  0, 255, 255), cv::Scalar(255, 255,   0),
        cv::Scalar(128,   0, 255),
    };
    const int max_w = img.cols, max_h = img.rows;
    for (int i = 0; i < r.object_count; ++i) {
        const AlgObject& o = r.objects[i];
        if (!(o.field_mask & ALG_FIELD_BOX)) continue;
        const AlgBox& b = o.box;
        const cv::Scalar& c = kColors[(b.label >= 0 ? b.label : 0) %
                                       (int)(sizeof(kColors)/sizeof(kColors[0]))];
        cv::Rect rect(b.xmin, b.ymin, b.xmax - b.xmin, b.ymax - b.ymin);
        rect &= cv::Rect(0, 0, max_w, max_h);
        if (rect.width <= 0 || rect.height <= 0) continue;
        cv::rectangle(img, rect, c, 2);

        char buf[96];
        const char* cls_str = (o.attributes && o.attributes->count > 0)
                                  ? o.attributes->items[0].value_str
                                  : "";
        if (cls_str && *cls_str)
            std::snprintf(buf, sizeof(buf), "%s %.2f", cls_str, b.score);
        else
            std::snprintf(buf, sizeof(buf), "label=%d %.2f", b.label, b.score);
        cv::putText(img, buf, cv::Point(rect.x, std::max(15, rect.y - 4)),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, c, 1);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "usage: %s <solution.json> <image.jpg> [out_dir]\n", argv[0]);
        return 1;
    }
    const char* json_path = argv[1];
    const char* image_path = argv[2];
    const char* out_dir = argc >= 4 ? argv[3] : "out";

    cv::Mat bgr = cv::imread(image_path, cv::IMREAD_COLOR);
    if (bgr.empty()) { std::fprintf(stderr, "imread failed: %s\n", image_path); return 1; }
    std::printf("[image ] %s (%dx%d)\n", image_path, bgr.cols, bgr.rows);
    std::printf("[sdk   ] %s\n", AlgVersion());

    AlgHandle h = nullptr;
    AlgStatus s = AlgCreate(&h, json_path);
    if (s != ALG_OK) { std::fprintf(stderr, "AlgCreate err %d\n", s); return s; }

    AlgImage img;
    img.format   = ALG_PIX_BGR;
    img.width    = bgr.cols;
    img.height   = bgr.rows;
    img.stride   = static_cast<int>(bgr.step);
    img.data_len = static_cast<int>(bgr.step) * bgr.rows;
    img.data     = bgr.data;

    AlgResult r{};
    s = AlgRun(h, &img, &r);
    if (s != ALG_OK) {
        std::fprintf(stderr, "AlgRun err %d\n", s);
        AlgDestroy(h);
        return s;
    }

    std::printf("[result] frame_id=%lld objects=%d\n",
                (long long)r.frame_id, r.object_count);
    for (int i = 0; i < r.object_count; ++i) {
        const AlgObject& o = r.objects[i];
        const AlgBox& b = o.box;
        std::printf("  [%d] label=%d score=%.3f box=(%d,%d)-(%d,%d)",
                    i, b.label, b.score, b.xmin, b.ymin, b.xmax, b.ymax);
        if (o.attributes && o.attributes->count > 0) {
            const AlgAttribute& a = o.attributes->items[0];
            std::printf("  attr[0]={name=%s,int=%d,float=%.3f,str=%s}",
                        a.name, a.value_int, a.value_float, a.value_str);
        }
        std::printf("\n");
    }

    /* 画框落盘。 */
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
