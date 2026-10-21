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
 *   -split-plane        把 Y 和 UV/VU 复制到两个独立缓冲，验证分地址输入
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
#include <string>
#include <vector>
#include "alg_interface.h"
#include "opencv2/opencv.hpp"
#include "yuv_tool.hpp" /* -scale：送 AlgRun 前的 CPU 侧 YUV420SP 下采样 */
#ifdef ALG_TEST_HAVE_LIBYUV
#include "libyuv/scale.h" /* NV12Scale(kFilterBox)：Y + 交错 UV 一步缩放 */
#endif

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
                     "  -split-plane      将 Y 与 UV/VU 复制到两个独立缓冲传入\n"
                     "  -save-every <N>   每 N 帧保存画框结果，0=从不保存（默认 0）\n"
                     "  -stats-every <N>  每 N 帧打印窗口统计（默认 100）\n"
                     "  -fps <N>          帧率上限：usleep 补齐到 1000/N ms/帧（0=不限速，默认 0）\n"
                     "  -v                每帧打印完整检出明细\n"
                     "  -log <level>      SDK 日志等级：error|warn|info|debug（默认保持编译期默认值）\n"
                     "                    info 会打印每模型的 pre/infer/post 及 AIPP 子阶段耗时\n"
                     "  -aipp-scale <m>   DV500 上缩放由谁做：auto（SDK 内部 libyuv 预缩放，默认）\n"
                     "                    | external（调用方/VPSS 已缩好，尺寸不符直接报错）\n"
                     "                    给了 -scale 时默认隐含 external，避免 SDK 二次缩放\n"
                     "  -scale <WxH>      送 AlgRun 前先用 CPU 把 yuv 缩放到该尺寸\n"
                     "                    DV500 上让 AIPP 做大比例缩放极慢，传 aipp_output_size\n"
                     "                    （车牌检测为 640x360）即可让 AIPP 只做转色\n"
                     "  -scale-mode <m>   缩放算法：libyuv（NEON 区域平均，最快）\n"
                     "                    | opencv（cv::resize INTER_AREA）\n"
                     "                    | box（区域平均的标量实现，慢但无依赖）\n"
                     "                    | nearest（最近邻，最快但抽稀像素，会掉识别率）\n"
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

    /* 日志等级：不传就沿用编译期默认值，便于按需打开分段耗时而不必重新编译动态库。 */
    const std::string log_opt = Opt(args, "-log", "");
    if (!log_opt.empty()) {
        AlgLogLevel level = ALG_LOG_WARN;
        if (log_opt == "error")
            level = ALG_LOG_ERROR;
        else if (log_opt == "warn")
            level = ALG_LOG_WARN;
        else if (log_opt == "info")
            level = ALG_LOG_INFO;
        else if (log_opt == "debug")
            level = ALG_LOG_DEBUG;
        else {
            std::fprintf(stderr, "bad -log: %s（期望 error|warn|info|debug）\n", log_opt.c_str());
            return 1;
        }
        AlgSetLogLevel(level);
        std::printf("[log   ] SDK 日志等级 -> %s\n", log_opt.c_str());
    }

    /* 动态 AIPP 缩放的执行方（仅 DV500 生效）。便于按 CPU 余量在板上切。
     *
     * 与 -scale 的关系：本程序自己缩过之后，必须让 SDK 别再缩，否则会缩两次
     * （结果仍正确，但白损画质、白花 CPU，而且是静默的）。所以给了 -scale 就默认
     * 隐含 external —— 此时如果缩到的尺寸不是模型的 aipp_output_size，SDK 会直接
     * 报错，而不是闷声二次缩放。要对比不同缩放内核时显式传 -aipp-scale auto。 */
    std::string aipp_scale_opt = Opt(args, "-aipp-scale", "");
    const bool aipp_scale_explicit = !aipp_scale_opt.empty();
    if (!aipp_scale_explicit && HasOpt(args, "-scale")) {
        aipp_scale_opt = "external";
        std::printf("[aipp  ] 已带 -scale，隐含 -aipp-scale external（避免 SDK 二次缩放）\n");
    }
    if (!aipp_scale_opt.empty()) {
        AlgAippScaleMode m = ALG_AIPP_SCALE_AUTO;
        if (aipp_scale_opt == "auto")
            m = ALG_AIPP_SCALE_AUTO;
        else if (aipp_scale_opt == "external")
            m = ALG_AIPP_SCALE_EXTERNAL;
        else {
            std::fprintf(stderr, "bad -aipp-scale: %s（期望 auto|external）\n", aipp_scale_opt.c_str());
            return 1;
        }
        const AlgStatus as = AlgSetAippScaleMode(m);
        if (as != ALG_OK) {
            std::fprintf(stderr, "AlgSetAippScaleMode err %d\n", as);
            return as;
        }
        std::printf("[aipp  ] 缩放执行方 -> %s\n", aipp_scale_opt.c_str());
        if (aipp_scale_explicit && aipp_scale_opt == "auto" && HasOpt(args, "-scale")) {
            std::fprintf(stderr,
                         "警告: -scale 与 -aipp-scale auto 同时使用；若 -scale 的目标尺寸不等于\n"
                         "      模型的 aipp_output_size，SDK 会再缩一次（双重缩放）。\n");
        }
    }

    /* 输入判定：图片 vs 原始 YUV。扩展名 .yuv / .bin 一律按 YUV 处理。 */
    const std::string ext = [&]() {
        auto dot = input_path.find_last_of('.');
        return dot == std::string::npos ? std::string() : input_path.substr(dot + 1);
    }();
    const bool is_yuv = (ext == "yuv" || ext == "bin" || ext == "nv12" || ext == "nv21");

    /* 准备 AlgImage：图片读一次成 BGR；YUV 整文件读进内存，循环里复用同一 buffer。 */
    std::vector<uint8_t> yuv_bytes;    /* yuv 输入持有原始字节（保持存活） */
    std::vector<uint8_t> scaled_bytes; /* -scale 时的缩放结果缓冲（逐帧覆写） */
    std::vector<uint8_t> y_plane_bytes, uv_plane_bytes;
    bool do_scale = false;    /* 每帧送 AlgRun 前先做 CPU 缩放 */
    std::string scale_mode;   /* box | nearest */
    int src_w = 0, src_h = 0; /* yuv 文件的原始尺寸 */
    cv::Mat image_bgr;        /* 图片输入持有解码结果 */
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
        const long expect = static_cast<long>(w) * h * 3 / 2; /* I420：Y + UV 交错 */
        if (static_cast<long>(yuv_bytes.size()) != expect) {
            std::fprintf(stderr, "yuv 文件大小 %zu 与 %d x %d NV12/NV21 期望 %ld 不符（可能含多帧或非 420）\n",
                         yuv_bytes.size(), w, h, expect);
            return 1;
        }
        img.stride = 0; /* NV12/NV21 按 width 紧凑布局 */
        const size_t y_size = static_cast<size_t>(w) * h;
        src_w = w;
        src_h = h;

        /* 可选：先用 CPU 把整帧缩到模型需要的尺寸再送 AlgRun。
         * DV500 的 AIPP 做大比例缩放（1080P -> 640x360）有硬件瓶颈，把缩放挪到
         * 调用方、让 AIPP 只做转色（scf_switch=0）可规避。这里用 yuv_tool.hpp 的
         * 最近邻实现，并在循环里逐帧执行，以便量出真实的每帧 CPU 开销。 */
        int sw = 0, sh = 0;
        const std::string scale_opt = Opt(args, "-scale");
        if (!scale_opt.empty()) {
            if (std::sscanf(scale_opt.c_str(), "%dx%d", &sw, &sh) != 2 || sw <= 0 || sh <= 0 || (sw & 1) || (sh & 1)) {
                std::fprintf(stderr, "bad -scale: %s（期望偶数 WxH，如 640x360）\n", scale_opt.c_str());
                return 1;
            }
            if (sw > w || sh > h) {
                std::fprintf(stderr, "-scale %s 不小于源尺寸 %dx%d，本工具只做下采样\n", scale_opt.c_str(), w, h);
                return 1;
            }
            scaled_bytes.resize(static_cast<size_t>(sw) * sh * 3 / 2);
            scale_mode = Opt(args, "-scale-mode", "libyuv");
            if (scale_mode != "libyuv" && scale_mode != "opencv" && scale_mode != "box" && scale_mode != "nearest") {
                std::fprintf(stderr, "bad -scale-mode: %s（期望 libyuv|opencv|box|nearest）\n", scale_mode.c_str());
                return 1;
            }
#ifndef ALG_TEST_HAVE_LIBYUV
            if (scale_mode == "libyuv") {
                std::fprintf(stderr, "-scale-mode libyuv 不可用（本次构建没链 libyuv），改用 opencv\n");
                scale_mode = "opencv";
            }
#endif
            do_scale = true;
        }

        img.data = nullptr;
        if (do_scale) {
            /* 缩放结果是一块连续堆缓冲，仍按分平面兼容路径传入：普通堆地址不能
             * 当作 MMZ/VB 的外部零拷贝输入。 */
            const size_t sy = static_cast<size_t>(sw) * sh;
            img.width = sw;
            img.height = sh;
            img.plane_data[0] = scaled_bytes.data();
            img.plane_data[1] = scaled_bytes.data() + sy;
            img.plane_data_len[0] = static_cast<int>(sy);
            img.plane_data_len[1] = static_cast<int>(scaled_bytes.size() - sy);
            img.plane_stride[0] = sw;
            img.plane_stride[1] = sw;
        } else if (HasOpt(args, "-split-plane")) {
            y_plane_bytes.assign(yuv_bytes.begin(), yuv_bytes.begin() + y_size);
            uv_plane_bytes.assign(yuv_bytes.begin() + y_size, yuv_bytes.end());
            img.plane_data[0] = y_plane_bytes.data();
            img.plane_data[1] = uv_plane_bytes.data();
            img.plane_data_len[0] = static_cast<int>(y_plane_bytes.size());
            img.plane_data_len[1] = static_cast<int>(uv_plane_bytes.size());
            img.plane_stride[0] = w;
            img.plane_stride[1] = w;
        } else {
            /* 文件缓冲不是 MMZ/VB 映射，按分平面兼容路径传入，避免测试程序
             * 把普通堆地址误用为动态 AIPP 的外部零拷贝输入。 */
            img.plane_data[0] = yuv_bytes.data();
            img.plane_data[1] = yuv_bytes.data() + y_size;
            img.plane_data_len[0] = static_cast<int>(y_size);
            img.plane_data_len[1] = static_cast<int>(yuv_bytes.size() - y_size);
            img.plane_stride[0] = w;
            img.plane_stride[1] = w;
        }
        std::printf("[input ] yuv %s (%dx%d, %s, %s)\n", input_path.c_str(), w, h, Opt(args, "-fmt", "nv12").c_str(),
                    HasOpt(args, "-split-plane") ? "split-plane" : "file-plane-view");
        if (do_scale) {
            const char* mode_name = scale_mode == "nearest" ? "最近邻"
                : scale_mode == "box"                       ? "区域平均(标量)"
                : scale_mode == "libyuv"                    ? "libyuv NV12Scale(kFilterBox)"
                                                            : "OpenCV INTER_AREA";
            std::printf("[scale ] 每帧 CPU 缩放到 %dx%d（%s），再送 AlgRun\n", sw, sh, mode_name);
        }
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
    /* -scale-mode opencv 用的可复用缓冲：只在第一帧分配，之后 cv::resize 原地复用。
     * Y 与 UV 分开缩放。UV 必须按“UV 对”来缩放，即 CV_8UC2、宽 src_w/2：若退化成
     * 宽 src_w 的单通道图，INTER_AREA 的 3 字节窗口会是 [V0,U0,V1]，把 U 和 V 混
     * 在一起平均，颜色就错了。CV_8UC2 让 cv::resize 对两个通道分别求平均。 */
    cv::Mat scale_y_src, scale_y_dst, scale_uv_src, scale_uv_dst;
    if (do_scale && scale_mode == "opencv") {
        scale_y_src = cv::Mat(src_h, src_w, CV_8UC1, yuv_bytes.data());
        scale_uv_src = cv::Mat(src_h / 2, src_w / 2, CV_8UC2, yuv_bytes.data() + static_cast<size_t>(src_w) * src_h);
    }

    timeval t2, t3, t4, t5, t6;
    while (!g_stop && (total <= 0 || frame < total)) {
        double scale_ms = 0.0, scale_y_ms = 0.0, scale_uv_ms = 0.0;
        if (do_scale) {
            gettimeofday(&t2, nullptr);
            if (scale_mode == "nearest") {
                FastYuv420spResize(yuv_bytes.data(), src_w, src_h, scaled_bytes.data(), img.width, img.height,
                                   img.format == ALG_PIX_NV21);
            } else if (scale_mode == "box") {
                FastYuv420spResizeBox(yuv_bytes.data(), src_w, src_h, scaled_bytes.data(), img.width, img.height,
                                      img.format == ALG_PIX_NV21);
            } else if (scale_mode == "opencv") {
                /* 分开计时，方便判断哪一部分慢（Y 占大头还是 UV 走了慢路径）。 */
                gettimeofday(&t4, nullptr);
                const size_t y_dst = static_cast<size_t>(img.width) * img.height;
                cv::resize(scale_y_src, scale_y_dst, cv::Size(img.width, img.height), 0, 0, cv::INTER_AREA);
                gettimeofday(&t5, nullptr);
                std::memcpy(scaled_bytes.data(), scale_y_dst.data, y_dst);
                cv::resize(scale_uv_src, scale_uv_dst, cv::Size(img.width / 2, img.height / 2), 0, 0, cv::INTER_AREA);
                std::memcpy(scaled_bytes.data() + y_dst, scale_uv_dst.data,
                            static_cast<size_t>(img.width) * (img.height / 2));
                gettimeofday(&t6, nullptr);
                scale_y_ms = Ms(t4, t5);
                scale_uv_ms = Ms(t5, t6);
            }
#ifdef ALG_TEST_HAVE_LIBYUV
            else {
                /* NV12Scale 内部把 UV 平面按“UV 对”（width/2）缩放，不解释 U/V 字节
                 * 语义，所以对 NV12 和 NV21 都正确 —— NV21 只是对内字节顺序不同。
                 * 一步到位，不需要先拆 I420（那会多 6.8MB/帧的读写）。 */
                const int sw_ = src_w, sh_ = src_h, dw_ = img.width, dh_ = img.height;
                libyuv::NV12Scale(yuv_bytes.data(), sw_, yuv_bytes.data() + static_cast<size_t>(sw_) * sh_, sw_, sw_,
                                  sh_, scaled_bytes.data(), dw_, scaled_bytes.data() + static_cast<size_t>(dw_) * dh_,
                                  dw_, dw_, dh_, libyuv::kFilterBox);
            }
#endif
            gettimeofday(&t3, nullptr);
            scale_ms = Ms(t2, t3);
        }

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

        const double infer_ms = Ms(t0, t1);
        /* 统计口径是"每帧端到端"：开了 -scale 就含缩放，否则与原来完全一致。 */
        const double ms = scale_ms + infer_ms;

        /* 帧率控制：帧间隔不足 1/fps 秒时 usleep 补齐（仅上限，推理更慢则不强制）。
         * 用整数微秒避免 softfp 板端浮点开销。 */
        if (period_us > 0) {
            const long used_us = static_cast<long>(ms * 1000.0);
            const long sleep_us = period_us - used_us;
            if (sleep_us > 0)
                usleep(static_cast<useconds_t>(sleep_us));
        }

        win_ms += ms;
        all_ms += ms;
        if (ms < win_min)
            win_min = ms;
        if (ms > win_max)
            win_max = ms;

        if (do_scale) {
            /* opencv 模式额外打出 Y/UV 两段的耗时：22ms 这种量级下需要知道
             * 是 Y 平面本身慢，还是 UV 走到了没有 SIMD 的慢路径。 */
            if (scale_mode == "opencv")
                std::printf("[frame %ld] scale: %6.2f ms (y %5.2f uv %5.2f)  infer: %7.2f ms  "
                            "total: %7.2f ms  SL=%d SG=%d LP=%d\n",
                            frame, scale_ms, scale_y_ms, scale_uv_ms, infer_ms, ms, r.speed_limit_count, r.sign_count,
                            r.license_plate_count);
            else
                std::printf("[frame %ld] scale: %6.2f ms  infer: %7.2f ms  total: %7.2f ms  "
                            "SL=%d SG=%d LP=%d\n",
                            frame, scale_ms, infer_ms, ms, r.speed_limit_count, r.sign_count, r.license_plate_count);
        } else
            std::printf("[frame %ld] infer: %7.2f ms  SL=%d SG=%d LP=%d\n", frame, infer_ms, r.speed_limit_count,
                        r.sign_count, r.license_plate_count);
        if (verbose)
            PrintResultDetails(r);

        /* 可选：每 N 帧存一张画框结果（默认不保存）。 */
        if (save_every > 0 && frame % save_every == 0) {
            cv::Mat draw;
            if (is_yuv) {
                /* 画框要看实际送进 AlgRun 的那一帧（开 -scale 时是缩放后的）。 */
                uint8_t* frame_base = do_scale ? scaled_bytes.data() : yuv_bytes.data();
                cv::Mat yuv_raw(img.height * 3 / 2, img.width, CV_8UC1, frame_base);
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
