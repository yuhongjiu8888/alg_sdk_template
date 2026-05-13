/**
 * @file test_facedet.cpp
 * @brief alg_sdk 多模型流水线的烟雾测试。
 *
 *   ./test_facedet <config.json> <image_path_or_dir> [out_dir]
 */

#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <opencv2/opencv.hpp>

#include "alg_interface.h"

static double now_ms() {
    timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static bool is_dir(const char* p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static void fill_image_bgr(const cv::Mat& m, AlgImage* img) {
    img->format = ALG_PIX_BGR;
    img->width  = m.cols;
    img->height = m.rows;
    img->stride = m.cols * 3;
    img->data_len = m.rows * m.cols * 3;
    img->data = m.data;
}

static void print_object(int i, const AlgObject& o) {
    printf("  [%d] mask=0x%x", i, o.field_mask);
    if (o.field_mask & ALG_FIELD_BOX) {
        printf("  box=(%d,%d,%d,%d) score=%.3f label=%d",
               o.box.xmin, o.box.ymin, o.box.xmax, o.box.ymax, o.box.score, o.box.label);
    }
    if ((o.field_mask & ALG_FIELD_KEYPOINTS) && o.keypoints) {
        printf("  kps=%d", o.keypoints->count);
    }
    if ((o.field_mask & ALG_FIELD_ATTRIBUTES) && o.attributes) {
        printf("  attrs={");
        for (int k = 0; k < o.attributes->count; ++k) {
            const AlgAttribute& a = o.attributes->items[k];
            printf("%s%s=", k ? "," : "", a.name);
            if (a.value_str[0]) printf("%s", a.value_str);
            else                printf("%.3f", a.value_float);
        }
        printf("}");
    }
    if ((o.field_mask & ALG_FIELD_EMBEDDING) && o.embedding) {
        printf("  emb_dim=%d", o.embedding->dim);
    }
    printf("\n");
}

static int run_one(AlgHandle h, const std::string& in, const std::string& out_path) {
    cv::Mat img = cv::imread(in);
    if (img.empty()) { fprintf(stderr, "cannot read %s\n", in.c_str()); return -1; }

    AlgImage image; fill_image_bgr(img, &image);
    AlgResult result{};

    double t0 = now_ms();
    AlgStatus s = AlgRun(h, &image, &result);
    double t1 = now_ms();
    if (s != ALG_OK) { fprintf(stderr, "AlgRun=%d\n", s); return -1; }

    printf("%s: objects=%d time=%.2fms\n", in.c_str(), result.object_count, t1 - t0);
    for (int i = 0; i < result.object_count; ++i) {
        const AlgObject& o = result.objects[i];
        print_object(i, o);
        if (o.field_mask & ALG_FIELD_BOX) {
            cv::rectangle(img, cv::Point(o.box.xmin, o.box.ymin),
                          cv::Point(o.box.xmax, o.box.ymax),
                          cv::Scalar(0, 255, 0), 2);
        }
        if ((o.field_mask & ALG_FIELD_KEYPOINTS) && o.keypoints) {
            for (int k = 0; k < o.keypoints->count; ++k) {
                cv::circle(img,
                           cv::Point((int)o.keypoints->xs[k], (int)o.keypoints->ys[k]),
                           2, cv::Scalar(0, 0, 255), -1);
            }
        }
    }
    if (!out_path.empty()) cv::imwrite(out_path, img);

    AlgFreeResult(&result);
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printf("Usage: %s <config.json> <image_or_dir> [out_dir]\n", argv[0]);
        return 1;
    }
    const char* cfg = argv[1];
    const char* in  = argv[2];
    const char* out_dir = (argc >= 4) ? argv[3] : "out";

    printf("SDK: %s (backend=%s)\n", AlgVersion(), AlgBackendName());

    AlgHandle h = nullptr;
    AlgStatus s = AlgCreate(&h, cfg);
    if (s != ALG_OK) { fprintf(stderr, "AlgCreate=%d\n", s); return 2; }

    mkdir(out_dir, 0755);

    if (is_dir(in)) {
        DIR* d = opendir(in);
        struct dirent* e;
        while (d && (e = readdir(d))) {
            std::string name = e->d_name;
            if (name == "." || name == "..") continue;
            std::string p = std::string(in) + "/" + name;
            std::string o = std::string(out_dir) + "/" + name;
            run_one(h, p, o);
        }
        if (d) closedir(d);
    } else {
        run_one(h, in, std::string(out_dir) + "/result.jpg");
    }

    AlgDestroy(h);
    return 0;
}
