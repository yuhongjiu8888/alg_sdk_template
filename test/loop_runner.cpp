/**
 * @file loop_runner.cpp
 * @brief 循环压测 runner：同一帧数据（图片或 NV12/NV21 yuv.bin）持续喂给 SDK 连续推理。
 *
 * 用途：板端稳定性 / 性能压测 —— AlgCreate 只做一次，AlgRun 在循环里复用同一
 * 个 AlgImage（与真实相机帧复用同一缓存一致），默认不保存结果图，避免磁盘 I/O
 * 污染每帧耗时测量。Ctrl-C 结束并打印汇总统计。
 *
 * 用法：
 *   ./loop_runner <solution.json> <input.jpg|input.yuv> [选项]
 *     图片输入：   ./loop_runner speed_limit.json /data/test.jpg -n 1000
 *     YUV 输入：   ./loop_runner license_plate.json /data/raw_1920x1080_nv12.yuv -n 0
 *                  （-n 0 / 缺省 = 无限循环直到 Ctrl-C）
 *
 * 选项：
 *   -n <count>          循环次数；0 或缺省 = 无限循环（Ctrl-C 停止）
 *   -size <WxH>         yuv 输入必需（除非文件名含 "WxH" 模式，如 raw_1920x1080_nv12.yuv）
 *   -fmt <nv12|nv21>    yuv 像素格式，默认 nv12
 *   -save-every <N>     每 N 帧保存一次画框结果（默认 0 = 从不保存；目录 loop_out/）
 *   -stats-every <N>    每 N 帧打印一次窗口统计（默认 100）
 *   -fps <N>            帧率上限：每帧耗时不足 1000/N ms 时 usleep 补齐（默认 0 = 不限速）
 *                       例：-fps 15 模拟 15fps 相机，-fps 10 模拟 10fps
 *   -v                  每帧打印完整检出明细（SL/SG/LP 列表）
 */

#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <string>
#include <vector>
#include "alg_interface.h"

namespace {

volatile sig_atomic_t g_stop = 0;
void OnSignal(int) {
    g_stop = 1;
}

double Ms(const timeval& a, const timeval& b) {
    return (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_usec - a.tv_usec) / 1000.0;
}

const char* SignName(AlgSignType t) {
    switch (t) {
        case SIGN_NO_PARKING:
            return "no_parking";
        case SIGN_PARE:
            return "pare";
        case SIGN_INVALID:
        default:
            return "invalid";
    }
}

void DrawBoxLabel(cv::Mat& img, const AlgBox& b, const char* text, cv::Scalar c) {
    cv::Rect rect(b.xmin, b.ymin, b.xmax - b.xmin, b.ymax - b.ymin);
    rect &= cv::Rect(0, 0, img.cols, img.rows);
    if (rect.width <= 0 || rect.height <= 0)
        return;
    cv::rectangle(img, rect, c, 2);
    cv::putText(img, text, cv::Point(rect.x, std::max(15, rect.y - 4)), cv::FONT_HERSHEY_SIMPLEX, 0.5, c, 1);
}

/* 画框（仅 -save-every 使用；循环热路径不碰）。 */
void DrawResult(cv::Mat& img, const AlgResult& r) {
    char buf[64];
    for (int i = 0; i < r.speed_limit_count; ++i) {
        const AlgSpeedLimit& s = r.speed_limits[i];
        std::snprintf(buf, sizeof(buf), "SL.%d", s.value * 10);
        DrawBoxLabel(img, s.box, buf, cv::Scalar(0, 255, 0));
    }
    for (int i = 0; i < r.sign_count; ++i) {
        const AlgSign& g = r.signs[i];
        std::snprintf(buf, sizeof(buf), "%s", SignName(g.type));
        DrawBoxLabel(img, g.box, buf, cv::Scalar(0, 128, 255));
    }
    for (int i = 0; i < r.license_plate_count; ++i) {
        const AlgLicensePlate& lp = r.license_plates[i];
        std::snprintf(buf, sizeof(buf), "%s %.2f", lp.text, lp.box.score);
        DrawBoxLabel(img, lp.box, buf, cv::Scalar(0, 255, 0));
    }
}

/* 简易选项解析：仅按空格分隔的 argv，形如 "-key value" / "-key"。 */
std::string Opt(const std::vector<std::string>& args, const std::string& key, const std::string& def = "") {
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == key && i + 1 < args.size())
            return args[i + 1];
    }
    return def;
}

bool HasOpt(const std::vector<std::string>& args, const std::string& key) {
    for (const auto& a : args)
        if (a == key)
            return true;
    return false;
}

int SLKmh(AlgSpeedLimitValue v) {
    if (v >= SLV_10 && v <= SLV_120)
        return static_cast<int>(v) * 10;
    return 0;
}

AlgPixelFormat PixelFormatOf(const char* s) {
    return (s && std::strcmp(s, "nv21") == 0) ? ALG_PIX_NV21 : ALG_PIX_NV12;
}

/* 从文件名里找 "WxH" 模式（如 raw_1920x1080_nv12.yuv）→ true 并写回宽高。
 * 失败需由调用方用 -size 显式指定。 */
bool TryParseSizeFromName(const std::string& path, int* w, int* h) {
    const std::string base = path.substr(path.find_last_of('/') + 1);
    for (size_t i = 0; i + 2 < base.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(base[i])))
            continue;
        size_t j = i;
        while (j < base.size() && std::isdigit(static_cast<unsigned char>(base[j])))
            ++j;
        if (j < base.size() && base[j] == 'x') {
            size_t k = j + 1;
            while (k < base.size() && std::isdigit(static_cast<unsigned char>(base[k])))
                ++k;
            if (k > j + 1) {
                *w = std::atoi(base.substr(i, j - i).c_str());
                *h = std::atoi(base.substr(j + 1, k - j - 1).c_str());
                return *w > 0 && *h > 0;
            }
        }
    }
    return false;
}

void PrintResultDetails(const AlgResult& r) {
    for (int i = 0; i < r.speed_limit_count; ++i) {
        const AlgSpeedLimit& sl = r.speed_limits[i];
        std::printf("                SL[%d] %d km/h score=%.3f\n", i, SLKmh(sl.value), sl.box.score);
    }
    for (int i = 0; i < r.sign_count; ++i) {
        const AlgSign& g = r.signs[i];
        std::printf("                SG[%d] %s score=%.3f\n", i, SignName(g.type), g.box.score);
    }
    for (int i = 0; i < r.license_plate_count; ++i) {
        const AlgLicensePlate& lp = r.license_plates[i];
        std::printf("                LP[%d] plate='%s' score=%.3f rec=%.3f\n", i, lp.text, lp.box.score, lp.rec_score);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "用法: %s <solution.json> <input.jpg|input.yuv> [选项]\n"
                     "  -n <count>        循环次数；0=无限循环直到 Ctrl-C（默认）\n"
                     "  -size <WxH>       yuv 输入必需（除非文件名含 WxH，如 raw_1920x1080_nv12.yuv）\n"
                     "  -fmt <nv12|nv21>  yuv 像素格式（默认 nv12）\n"
                     "  -save-every <N>   每 N 帧保存画框结果，0=从不保存（默认 0）\n"
                     "  -stats-every <N>  每 N 帧打印窗口统计（默认 100）\n"
                     "  -fps <N>          帧率上限：usleep 补齐到 1000/N ms/帧（0=不限速，默认 0）\n"
                     "  -v                每帧打印完整检出明细\n"
                     "示例: %s license_plate.json /data/raw_1920x1080_nv12.yuv -n 1000\n",
                     argv[0], argv[0]);
        return 1;
    }

    std::vector<std::string> args;
    for (int i = 3; i < argc; ++i)
        args.push_back(argv[i]);
    const std::string json_path = argv[1];
    const std::string input_path = argv[2];
    const long total = std::atol(Opt(args, "-n", "0").c_str());
    const int save_every = std::atoi(Opt(args, "-save-every", "0").c_str());
    const int stats_every = std::max(1, std::atoi(Opt(args, "-stats-every", "100").c_str()));
    const int fps = std::atoi(Opt(args, "-fps", "0").c_str());
    const long period_us = fps > 0 ? 1000000L / fps : 0; /* 每帧目标间隔（微秒） */
    const bool verbose = HasOpt(args, "-v");

    /* 输入判定：图片 vs 原始 YUV。扩展名 .yuv / .bin 一律按 YUV 处理。 */
    const std::string ext = [&]() {
        auto dot = input_path.find_last_of('.');
        return dot == std::string::npos ? std::string() : input_path.substr(dot + 1);
    }();
    const bool is_yuv = (ext == "yuv" || ext == "bin" || ext == "nv12" || ext == "nv21");

    /* 准备 AlgImage：图片读一次成 BGR；YUV 整文件读进内存，循环里复用同一 buffer。 */
    std::vector<uint8_t> yuv_bytes; /* yuv 输入持有原始字节（保持存活） */
    cv::Mat image_bgr;              /* 图片输入持有解码结果 */
    AlgImage img{};
    if (is_yuv) {
        FILE* f = std::fopen(input_path.c_str(), "rb");
        if (!f) {
            std::fprintf(stderr, "opend failed: %s\n", input_path.c_str());
            return 1;
        }
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size <= 0) {
            std::fprintf(stderr, "empty yuv file: %s\n", input_path.c_str());
            std::fclose(f);
            return 1;
        }
        yuv_bytes.resize(static_cast<size_t>(size));
        if (std::fread(yuv_bytes.data(), 1, yuv_bytes.size(), f) != yuv_bytes.size()) {
            std::fprintf(stderr, "read failed: %s\n", input_path.c_str());
            std::fclose(f);
            return 1;
        }
        std::fclose(f);

        int w = 0, h = 0;
        const std::string size_opt = Opt(args, "-size");
        if (!size_opt.empty()) {
            if (std::sscanf(size_opt.c_str(), "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0) {
                std::fprintf(stderr, "bad -size: %s（期望 WxH，如 1920x1080）\n", size_opt.c_str());
                return 1;
            }
        } else if (!TryParseSizeFromName(input_path, &w, &h)) {
            std::fprintf(stderr, "yuv 输入需要尺寸：文件名含 WxH 或传 -size <WxH>\n");
            return 1;
        }
        img.format = PixelFormatOf(Opt(args, "-fmt", "nv12").c_str());
        img.width = w;
        img.height = h;
        img.stride = 0; /* NV12/NV21 按 width 紧凑布局 */
        img.data = yuv_bytes.data();
        img.data_len = static_cast<int>(yuv_bytes.size());
        const long expect = static_cast<long>(w) * h * 3 / 2; /* I420：Y + UV 交错 */
        if (static_cast<long>(yuv_bytes.size()) != expect) {
            std::fprintf(stderr, "yuv 文件大小 %zu 与 %d x %d NV12/NV21 期望 %ld 不符（可能含多帧或非 420）\n",
                         yuv_bytes.size(), w, h, expect);
            return 1;
        }
        std::printf("[input ] yuv %s (%dx%d, %s)\n", input_path.c_str(), w, h, Opt(args, "-fmt", "nv12").c_str());
    } else {
        image_bgr = cv::imread(input_path, cv::IMREAD_COLOR);
        if (image_bgr.empty()) {
            std::fprintf(stderr, "imread failed: %s\n", input_path.c_str());
            return 1;
        }
        img.format = ALG_PIX_BGR;
        img.width = image_bgr.cols;
        img.height = image_bgr.rows;
        img.stride = static_cast<int>(image_bgr.step);
        img.data = image_bgr.data;
        img.data_len = static_cast<int>(image_bgr.step) * image_bgr.rows;
        std::printf("[input ] image %s (%dx%d)\n", input_path.c_str(), image_bgr.cols, image_bgr.rows);
    }

    std::printf("[sdk   ] %s\n", AlgVersion());

    AlgHandle h = nullptr;
    timeval t0, t1;
    gettimeofday(&t0, nullptr);
    AlgStatus s = AlgCreate(&h, json_path.c_str());
    gettimeofday(&t1, nullptr);
    if (s != ALG_OK) {
        std::fprintf(stderr, "AlgCreate err %d\n", s);
        return s;
    }
    std::printf("[init  ] %.2f ms\n", Ms(t0, t1));

    if (save_every > 0)
        ::mkdir("loop_out", 0755);
    std::signal(SIGINT, OnSignal);
    std::printf("[loop  ] %s(total=%ld) 保存: %s 帧率: %s\n", total > 0 ? "有限" : "无限", total,
                save_every > 0 ? "每 N 帧" : "从不", fps > 0 ? Opt(args, "-fps").c_str() : "不限");

    long frame = 0, failures = 0;
    double win_ms = 0, win_min = 1e30, win_max = -1e30, all_ms = 0;
    while (!g_stop && (total <= 0 || frame < total)) {
        gettimeofday(&t0, nullptr);
        AlgResult r{};
        AlgStatus s = AlgRun(h, &img, &r);
        gettimeofday(&t1, nullptr);
        if (s != ALG_OK) {
            std::fprintf(stderr, "[frame %ld] AlgRun err %d\n", frame, s);
            ++failures;
            if (failures >= 10) {
                std::fprintf(stderr, "连续失败过多，退出\n");
                break;
            }
            continue;
        }

        /* 帧率控制：帧间隔不足 1/fps 秒时 usleep 补齐（仅上限，推理更慢则不强制）。
         * 用整数微秒避免 softfp 板端浮点开销。 */
        if (period_us > 0) {
            const long infer_us = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_usec - t0.tv_usec);
            const long sleep_us = period_us - infer_us;
            if (sleep_us > 0)
                usleep(static_cast<useconds_t>(sleep_us));
        }

        const double ms = Ms(t0, t1);
        win_ms += ms;
        all_ms += ms;
        if (ms < win_min)
            win_min = ms;
        if (ms > win_max)
            win_max = ms;

        std::printf("[frame %ld] infer: %7.2f ms  SL=%d SG=%d LP=%d\n", frame, ms, r.speed_limit_count, r.sign_count,
                    r.license_plate_count);
        if (verbose)
            PrintResultDetails(r);

        /* 可选：每 N 帧存一张画框结果（默认不保存）。 */
        if (save_every > 0 && frame % save_every == 0) {
            cv::Mat draw;
            if (is_yuv) {
                cv::Mat yuv_raw(img.height * 3 / 2, img.width, CV_8UC1,
                                const_cast<uint8_t*>(static_cast<const uint8_t*>(img.data)));
                cv::cvtColor(yuv_raw, draw,
                             img.format == ALG_PIX_NV21 ? cv::COLOR_YUV2BGR_NV21 : cv::COLOR_YUV2BGR_NV12);
                DrawResult(draw, r);
            } else {
                draw = image_bgr.clone();
                DrawResult(draw, r);
            }
            char out_path[256];
            std::snprintf(out_path, sizeof(out_path), "loop_out/frame_%05ld.jpg", frame);
            cv::imwrite(out_path, draw);
            std::printf("        [save  ] %s\n", out_path);
        }
        ++frame;
        if (frame % stats_every == 0) {
            std::printf("[stats ] %ld frames: avg %.2f ms  min %.2f  max %.2f  fps %.2f\n", frame, win_ms / stats_every,
                        win_min, win_max, 1000.0 / (win_ms / stats_every));
            win_ms = 0;
            win_min = 1e30;
            win_max = -1e30;
        }
    }

    /* 汇总（窗口未对齐时补一段尾巴；无限循环被 Ctrl-C 打断也能看到汇总）。 */
    const long win_frames = frame % stats_every;
    if (win_frames > 0) {
        std::printf("[stats ] 尾段 %ld frames: avg %.2f ms  min %.2f  max %.2f  fps %.2f\n", win_frames,
                    win_ms / win_frames, win_min, win_max, 1000.0 / (win_ms / win_frames));
    }
    if (frame > 0) {
        std::printf("[done  ] %s：%ld 帧，平均 %.2f ms，fps %.2f，失败 %ld\n", g_stop ? "被 Ctrl-C 中断" : "完成",
                    frame, all_ms / frame, 1000.0 / (all_ms / frame), failures);
    }

    AlgDestroy(h);
    return failures == 0 ? 0 : 1;
}
