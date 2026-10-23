#include "alg_interface.h"
#include <cerrno>
#include <ctime>
#include <new>
#include <opencv2/core.hpp>
#include <string>
#include <vector>
#include "backend/backend_factory.h"
#include "core/config/config.h"
#include "core/image_view.h"
#include "core/infer/aipp_scale.h"
#include "core/logger.h"
#include "core/object.h"
#include "core/solution/chain_solution.h"

#define ALG_VERSION_STRING "alg_sdk.v3.0.0"

namespace {

struct Context {
    alg::ChainSolution solution;
    std::vector<alg::Object> objects_scratch;
    int target_fps = 0;            /* 0 = 不限速 */
    timespec next_start{};         /* 下一帧允许开始处理的单调时钟时间点 */
    bool has_next_start = false;
};

inline bool TimespecLess(const timespec& a, const timespec& b) {
    return a.tv_sec < b.tv_sec || (a.tv_sec == b.tv_sec && a.tv_nsec < b.tv_nsec);
}

inline void NormalizeTimespec(timespec* ts) {
    while (ts->tv_nsec >= 1000000000L) {
        ts->tv_nsec -= 1000000000L;
        ++ts->tv_sec;
    }
}

/* 睡到绝对时间点；clock_nanosleep 被信号打断时继续等剩余时间（ABSTIME 语义）。 */
inline void SleepUntil(const timespec& deadline) {
    int rc = 0;
    do {
        rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr);
    } while (rc == EINTR);
}

/* AlgRun 内部限速：同步接口下，集成方在上一次返回前不会再调，睡在这里
 * 就能约束实际处理节奏。返回本次处理的起始时间（单调时钟）。 */
void RateLimitBeforeRun(Context* ctx) {
    if (!ctx || ctx->target_fps <= 0)
        return;
    const long interval_ns = 1000000000L / ctx->target_fps;
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!ctx->has_next_start) {
        ctx->has_next_start = true;
        ctx->next_start = now;
    }
    if (TimespecLess(now, ctx->next_start)) {
        SleepUntil(ctx->next_start);
        clock_gettime(CLOCK_MONOTONIC, &now);
    }

    /* 按“计划时间点 + 间隔”推进，而不是用实际醒来时间重新起算。
     * 板端定时器粒度可能有几个 ms（实测 10fps 时每次多睡约 10ms），
     * 用 now+interval 会把这部分误差逐帧累积，10fps 变成 9.1fps。
     * 现在迟到只影响本次，下一次仍按原计划推进。 */
    ctx->next_start.tv_nsec += interval_ns;
    NormalizeTimespec(&ctx->next_start);
    if (TimespecLess(ctx->next_start, now)) {
        /* 处理/唤醒已经落后超过一个周期：跳过错过的时间点，不突发追赶。 */
        ctx->next_start = now;
        ctx->next_start.tv_nsec += interval_ns;
        NormalizeTimespec(&ctx->next_start);
    }
}

}  // namespace

extern "C" {

AlgStatus AlgCreate(AlgHandle* handle, const char* config_json_path) {
    if (!handle || !config_json_path)
        return ALG_E_INVALID_ARG;

    /* 端侧 OpenCV 只处理小图（96x96、256x64、ROI）。OpenCV 的 pthread 并行
     * 会在首帧创建线程池并做并行同步，开销往往大于收益，还会和其它线程抢 CPU。
     * SDK 本身按串行调用设计，这里固定单线程；集成方若另有并行需求，可在
     * AlgCreate 之后自行调用 cv::setNumThreads 恢复。 */
    cv::setNumThreads(1);

    alg::SolutionConfig cfg;
    std::string err;
    AlgStatus s = alg::LoadSolutionConfig(config_json_path, &cfg, &err);
    if (s != ALG_OK)
        return s;
    if (cfg.solution_type != "chain") {
        ALG_LOGE("only solution.type == 'chain' is supported, got '%s'", cfg.solution_type.c_str());
        return ALG_E_CONFIG;
    }

    auto* ctx = new (std::nothrow) Context();
    if (!ctx)
        return ALG_E_OOM;
    ctx->target_fps = cfg.target_fps;

    s = ctx->solution.Init(cfg);
    if (s != ALG_OK) {
        delete ctx;
        return s;
    }

    *handle = reinterpret_cast<AlgHandle>(ctx);
    return ALG_OK;
}

AlgStatus AlgDestroy(AlgHandle handle) {
    if (!handle)
        return ALG_E_INVALID_ARG;
    delete reinterpret_cast<Context*>(handle);
    return ALG_OK;
}

AlgStatus AlgRun(AlgHandle handle, const AlgImage* image, AlgResult* result) {
    if (!handle || !image || !result)
        return ALG_E_INVALID_ARG;

    alg::ImageView source_view;
    AlgStatus image_status = alg::ResolveImageView(*image, &source_view);
    if (image_status != ALG_OK)
        return image_status;

    auto* ctx = reinterpret_cast<Context*>(handle);
    RateLimitBeforeRun(ctx);
    result->speed_limit_count = 0;
    result->sign_count = 0;
    result->license_plate_count = 0;
    AlgStatus s = ctx->solution.Run(*image, &ctx->objects_scratch);
    if (s != ALG_OK)
        return s;
    alg::FillAlgResult(ctx->objects_scratch, result);

    static long long frame_counter = 0;
    result->frame_id = frame_counter++;
    return ALG_OK;
}

AlgStatus AlgSetLogLevel(AlgLogLevel level) {
    if (level < ALG_LOG_OFF || level > ALG_LOG_DEBUG)
        return ALG_E_INVALID_ARG;
    alg::log::set_level(static_cast<alg::log::Level>(level));
    return ALG_OK;
}

AlgStatus AlgSetTargetFps(AlgHandle handle, int fps) {
    if (!handle || fps < 0 || fps > 1000)
        return ALG_E_INVALID_ARG;
    auto* ctx = reinterpret_cast<Context*>(handle);
    ctx->target_fps = fps;
    /* 重新起算，避免沿用切换前的 deadline 造成一次多余的等待。 */
    ctx->has_next_start = false;
    return ALG_OK;
}

AlgStatus AlgSetAippScaleMode(AlgAippScaleMode mode) {
    if (mode < ALG_AIPP_SCALE_AUTO || mode > ALG_AIPP_SCALE_EXTERNAL)
        return ALG_E_INVALID_ARG;
    alg::aipp_scale::set_mode(mode == ALG_AIPP_SCALE_EXTERNAL ? alg::aipp_scale::Mode::kExternal
                                                              : alg::aipp_scale::Mode::kAuto);
    return ALG_OK;
}

const char* AlgVersion(void) {
    static std::string s = std::string(ALG_VERSION_STRING) + "+" + alg::BackendName();
    return s.c_str();
}

const char* AlgBackendName(void) {
    return alg::BackendName();
}

}  // extern "C"
