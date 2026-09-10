/**
 * @file test_runner.cpp
 * @brief 通用 SDK 烟雾测试：传入 JSON solution + 一张图或一个目录，跑完打印强类型结果。
 *
 * 同一个二进制可跑（红绿灯输出当前已屏蔽）：
 *   ./test_runner resources/speed_limit.json     /data/test.jpg out/
 *   ./test_runner resources/all.json             /data/test.jpg out/
 *   ./test_runner resources/config/svp_acl/license_plate.json /data/test.jpg out/
 *
 * 第二个参数也可以是目录，批量跑目录下所有图片（句柄只创建一次，复用跑全部）：
 *   ./test_runner resources/speed_limit.json     /data/imgs/  out/
 */

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "alg_interface.h"

namespace {

int SLKmh(AlgSpeedLimitValue v) {
    /* 新枚举按 km/h÷10 连续编号（SLV_10=1 … SLV_90=9 … SLV_120=12），value×10 即 km/h。
     * 旧实现漏了 SLV_90，落到 return 0 → 90 显示成 0。 */
    if (v >= SLV_10 && v <= SLV_120) return static_cast<int>(v) * 10;
    return 0;
}

const char* SignName(AlgSignType t) {
    switch (t) {
        case SIGN_NO_PARKING: return "no_parking";
        case SIGN_PARE:       return "pare";
        case SIGN_INVALID:
        default:              return "invalid";
    }
}

/* 限速牌按速度上色：越慢越绿、越快越红（绿→黄→红渐变），便于一眼区分不同限速。 */
cv::Scalar SLColor(AlgSpeedLimitValue v) {
    if (v < SLV_10 || v > SLV_120) return cv::Scalar(160, 160, 160);  /* 异常/INVALID：灰 */
    float t = static_cast<float>(static_cast<int>(v) - SLV_10) /
              static_cast<float>(SLV_120 - SLV_10);                   /* 10km/h→0 … 120km/h→1 */
    int g, r;
    if (t < 0.5f) { r = static_cast<int>(255 * t / 0.5f);            g = 255; }  /* 绿→黄 */
    else          { r = 255; g = static_cast<int>(255 * (1.0f - (t - 0.5f) / 0.5f)); }  /* 黄→红 */
    return cv::Scalar(0, g, r);  /* BGR */
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
    for (int i = 0; i < r.speed_limit_count; ++i) {
        const AlgSpeedLimit& s = r.speed_limits[i];
        std::snprintf(buf, sizeof(buf), "SL.%d %.2f", SLKmh(s.value), s.box.score);
        DrawBoxLabel(img, s.box, buf, SLColor(s.value));
    }
    for (int i = 0; i < r.sign_count; ++i) {
        const AlgSign& g = r.signs[i];
        std::snprintf(buf, sizeof(buf), "%s %.2f", SignName(g.type), g.box.score);
        DrawBoxLabel(img, g.box, buf, cv::Scalar(0, 128, 255));  /* 橙色：禁令/停车牌 */
    }
    for (int i = 0; i < r.license_plate_count; ++i) {
        const AlgLicensePlate& lp = r.license_plates[i];
        std::snprintf(buf, sizeof(buf), "%s %.2f", lp.text, lp.box.score);
        DrawBoxLabel(img, lp.box, buf, cv::Scalar(0, 255, 0));  /* 绿色：车牌 */
    }
}

bool IsDir(const char* path) {
    struct stat st;
    return ::stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool HasImageExt(const std::string& name) {
    auto dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "bmp" ||
           ext == "webp" || ext == "tif" || ext == "tiff";
}

/* 列出目录下所有图片文件（不递归），按文件名排序，方便结果稳定可对照。 */
std::vector<std::string> ListImages(const char* dir_path) {
    std::vector<std::string> files;
    DIR* dir = ::opendir(dir_path);
    if (!dir) return files;
    std::string base(dir_path);
    if (!base.empty() && base.back() == '/') base.pop_back();
    for (struct dirent* ent = ::readdir(dir); ent; ent = ::readdir(dir)) {
        std::string name(ent->d_name);
        if (name == "." || name == "..") continue;
        if (!HasImageExt(name)) continue;
        std::string full = base + "/" + name;
        if (IsDir(full.c_str())) continue;
        files.push_back(full);
    }
    ::closedir(dir);
    std::sort(files.begin(), files.end());
    return files;
}

/* 取路径文件名（不含目录），用于批量模式给每张图命名输出。 */
std::string BaseName(const std::string& path) {
    auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

/* 跑单张图：复用已创建的句柄，打印结果并保存画框图。返回 0 成功。 */
int RunOne(AlgHandle h, const std::string& image_path, const std::string& out_path) {
    cv::Mat bgr = cv::imread(image_path, cv::IMREAD_COLOR);
    if (bgr.empty()) { std::fprintf(stderr, "imread failed: %s\n", image_path.c_str()); return 1; }
    std::printf("[image ] %s (%dx%d)\n", image_path.c_str(), bgr.cols, bgr.rows);

    AlgImage img{};
    img.format   = ALG_PIX_BGR;
    img.width    = bgr.cols;
    img.height   = bgr.rows;
    img.stride   = static_cast<int>(bgr.step);
    img.data_len = static_cast<int>(bgr.step) * bgr.rows;
    img.data     = bgr.data;

    AlgResult r{};
    timeval t0, t1;
    gettimeofday(&t0, nullptr);
    AlgStatus s = AlgRun(h, &img, &r);
    gettimeofday(&t1, nullptr);
    if (s != ALG_OK) { std::fprintf(stderr, "AlgRun err %d\n", s); return s; }

    std::printf("[infer ] %.2f ms\n",
                (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_usec - t0.tv_usec) / 1000.0);
    std::printf("[result] frame_id=%lld  speed_limits=%d  signs=%d  license_plates=%d\n",
                (long long)r.frame_id, r.speed_limit_count, r.sign_count,
                r.license_plate_count);
    for (int i = 0; i < r.speed_limit_count; ++i) {
        const AlgSpeedLimit& sl = r.speed_limits[i];
        std::printf("  SL[%d] %d km/h score=%.3f  box=(%d,%d)-(%d,%d)\n",
                    i, SLKmh(sl.value), sl.box.score,
                    sl.box.xmin, sl.box.ymin, sl.box.xmax, sl.box.ymax);
    }
    for (int i = 0; i < r.sign_count; ++i) {
        const AlgSign& g = r.signs[i];
        std::printf("  SG[%d] %s score=%.3f  box=(%d,%d)-(%d,%d)\n",
                    i, SignName(g.type), g.box.score,
                    g.box.xmin, g.box.ymin, g.box.xmax, g.box.ymax);
    }
    for (int i = 0; i < r.license_plate_count; ++i) {
        const AlgLicensePlate& lp = r.license_plates[i];
        std::printf("  LP[%d] plate='%s' score=%.3f rec=%.3f  box=(%d,%d)-(%d,%d)\n",
                    i, lp.text, lp.box.score, lp.rec_score,
                    lp.box.xmin, lp.box.ymin, lp.box.xmax, lp.box.ymax);
    }

    DrawResult(bgr, r);
    if (!cv::imwrite(out_path, bgr))
        std::fprintf(stderr, "imwrite fail: %s\n", out_path.c_str());
    else
        std::printf("[save  ] %s\n", out_path.c_str());

    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "usage: %s <solution.json> <image_or_dir> [out_dir]\n", argv[0]);
        return 1;
    }
    const char* json_path  = argv[1];
    const char* input_path = argv[2];
    const char* out_dir    = argc >= 4 ? argv[3] : "out";

    /* 先把待跑图片列出来：目录则批量，单文件则单张。 */
    const bool batch = IsDir(input_path);
    std::vector<std::string> images;
    if (batch) {
        images = ListImages(input_path);
        if (images.empty()) {
            std::fprintf(stderr, "no image found in dir: %s\n", input_path);
            return 1;
        }
        std::printf("[batch ] %zu images under %s\n", images.size(), input_path);
    } else {
        images.push_back(input_path);
    }

    std::printf("[sdk   ] %s\n", AlgVersion());

    AlgHandle h = nullptr;
    timeval t0, t1;
    gettimeofday(&t0, nullptr);
    AlgStatus s = AlgCreate(&h, json_path);
    gettimeofday(&t1, nullptr);
    if (s != ALG_OK) { std::fprintf(stderr, "AlgCreate err %d\n", s); return s; }
    std::printf("[init  ] %.2f ms\n",
                (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_usec - t0.tv_usec) / 1000.0);

    mkdir(out_dir, 0755);

    /* 句柄只创建一次，循环复用跑全部图片。 */
    int failed = 0;
    for (size_t i = 0; i < images.size(); ++i) {
        if (batch) std::printf("\n[%zu/%zu] ----------------------------------------\n",
                               i + 1, images.size());
        /* 批量模式按原文件名输出（避免互相覆盖）；单张模式沿用 result.jpg。 */
        std::string out_path = std::string(out_dir) + "/" +
                               (batch ? BaseName(images[i]) : std::string("result.jpg"));
        if (RunOne(h, images[i], out_path) != 0) ++failed;
    }

    if (batch)
        std::printf("\n[done  ] %zu ok, %d failed\n", images.size() - failed, failed);

    AlgDestroy(h);
    return failed == 0 ? 0 : 1;
}
