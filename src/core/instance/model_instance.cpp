#include "core/instance/model_instance.h"
#include <sys/time.h>
#include <algorithm>
#include <ctime>
#include "backend/backend_factory.h"
#include "core/logger.h"
#include "core/preprocess/letterbox_preprocessor.h"
#include "core/registry/postprocessor_registry.h"

namespace alg {

namespace {
inline double Ms(const timeval& a, const timeval& b) {
    return (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_usec - a.tv_usec) / 1000.0;
}

/* 当前线程累计 CPU 时间：与墙钟对照可区分“在算”“在等 NPU”“在等内存”。 */
inline double ThreadCpuMs() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
}  // namespace

ModelInstance::ModelInstance() = default;
ModelInstance::~ModelInstance() = default;

Status ModelInstance::Init(const ModelInstanceConfig& cfg) {
    cfg_ = cfg;

    inferer_ = MakeInferer();
    if (!inferer_)
        return ALG_E_BACKEND;
    Status s = inferer_->Load(cfg.model_path);
    if (s != ALG_OK)
        return s;
    if (inferer_->NumInputs() < 1 || inferer_->NumOutputs() < 1) {
        ALG_LOGE("[%s] inferer reports no I/O tensors", cfg.name.c_str());
        return ALG_E_BACKEND;
    }

    if (inferer_->SupportsDynamicAipp()) {
        if (cfg.pre.engine == PreprocessEngine::kOpenCV) {
            ALG_LOGE("[%s] dynamic-AIPP OM cannot use preprocess.engine=opencv", cfg.name.c_str());
            return ALG_E_CONFIG;
        }
    } else {
        auto* lb = new LetterboxPreprocessor();
        pre_.reset(lb);
        lb_pre_ = cfg.pre.engine == PreprocessEngine::kAipp ? nullptr : lb;
        s = pre_->Configure(cfg.pre, inferer_->InputView(0));
        if (s != ALG_OK)
            return s;
    }

    post_ = PostprocessorRegistry::Instance().Create(cfg.post_type);
    if (!post_) {
        ALG_LOGE("[%s] postprocess type '%s' not registered", cfg.name.c_str(), cfg.post_type.c_str());
        return ALG_E_MODEL_NOT_FOUND;
    }
    s = post_->Configure(*inferer_, cfg.post_params);
    if (s != ALG_OK)
        return s;

    initialized_ = true;
    ALG_LOGI("[%s] model instance ready (post_type=%s, inputs=%d, outputs=%d)", cfg.name.c_str(), cfg.post_type.c_str(),
             inferer_->NumInputs(), inferer_->NumOutputs());
    return ALG_OK;
}

Status ModelInstance::Run(const AlgImage& image, std::vector<Object>* out, SharedInputStaging* shared_staging) {
    return RunImpl(image, out, shared_staging);
}

Status ModelInstance::RunNv12Roi(const AlgImage& image,
                                 const AlgBox& box,
                                 const CropConfig& crop,
                                 std::vector<Object>* out,
                                 CropTransform* xf) {
    if (!initialized_ || !lb_pre_ || !out || !xf)
        return ALG_E_INVALID_ARG;
    out->clear();

    timeval t0, t1, t2, t3;
    double c0 = 0.0, c1 = 0.0, c2 = 0.0;
    const bool profile = ALG_LOG_IS_ENABLED(alg::log::Level::Info);
    if (profile) {
        gettimeofday(&t0, nullptr);
        c0 = ThreadCpuMs();
    }

    PreprocessState state;
    Status s = lb_pre_->ApplyNv12Roi(image, box, crop, inferer_->InputView(0), state, xf);
    if (s != ALG_OK)
        return s;
    if (profile) {
        gettimeofday(&t1, nullptr);
        c1 = ThreadCpuMs();
    }

    s = inferer_->Forward();
    if (s != ALG_OK)
        return s;
    if (profile) {
        gettimeofday(&t2, nullptr);
        c2 = ThreadCpuMs();
    }

    s = post_->Apply(*inferer_, state, out);
    if (s != ALG_OK)
        return s;

    if (profile) {
        gettimeofday(&t3, nullptr);
        const double c3 = ThreadCpuMs();
        ALG_LOGI("[%s] pre=%.2fms pre_cpu=%.2fms infer=%.2fms infer_cpu=%.2fms post=%.2fms "
                 "post_cpu=%.2fms objects=%zu (fused_roi)",
                 cfg_.name.c_str(), Ms(t0, t1), c1 - c0, Ms(t1, t2), c2 - c1, Ms(t2, t3), c3 - c2, out->size());
    }
    return ALG_OK;
}

Status ModelInstance::RunImpl(const AlgImage& image, std::vector<Object>* out, SharedInputStaging* shared_staging) {
    if (!initialized_)
        return ALG_E_NOT_INITIALIZED;
    if (!out)
        return ALG_E_INVALID_ARG;
    out->clear();

    timeval t0, t1, t2, t3;
    double c0 = 0.0, c1 = 0.0, c2 = 0.0;
    const bool profile = ALG_LOG_IS_ENABLED(alg::log::Level::Info);
    if (profile) {
        gettimeofday(&t0, nullptr);
        c0 = ThreadCpuMs();
    }

    PreprocessState state;
    Status s;
    const bool try_aipp = cfg_.pre.engine != PreprocessEngine::kOpenCV && inferer_->SupportsDynamicAipp();
    if (try_aipp) {
        s = inferer_->PrepareDynamicAipp(image, cfg_.pre, state, shared_staging);
    } else if (cfg_.pre.engine == PreprocessEngine::kAipp) {
        ALG_LOGE("[%s] preprocess.engine=aipp but dynamic AIPP is unavailable", cfg_.name.c_str());
        return ALG_E_PREPROCESS;
    } else {
        if (!pre_)
            return ALG_E_PREPROCESS;
        s = pre_->Apply(image, inferer_->InputView(0), state);
    }
    if (s != ALG_OK)
        return s;
    if (profile) {
        gettimeofday(&t1, nullptr);
        c1 = ThreadCpuMs();
    }

    s = inferer_->Forward();
    if (s != ALG_OK)
        return s;
    if (profile) {
        gettimeofday(&t2, nullptr);
        c2 = ThreadCpuMs();
    }

    s = post_->Apply(*inferer_, state, out);
    if (s != ALG_OK)
        return s;
    if (profile) {
        gettimeofday(&t3, nullptr);
        const double c3 = ThreadCpuMs();
        const double pre_ms = Ms(t0, t1);
        ALG_LOGI("[%s] pre=%.2fms pre_cpu=%.2fms infer=%.2fms infer_cpu=%.2fms post=%.2fms "
                 "post_cpu=%.2fms objects=%zu",
                 cfg_.name.c_str(), pre_ms, c1 - c0, Ms(t1, t2), c2 - c1, Ms(t2, t3), c3 - c2, out->size());
        if (state.aipp_profile_valid) {
            const double measured_ms =
                state.aipp_setup_ms + state.aipp_bind_ms + state.aipp_copy_ms + state.aipp_flush_ms + state.aipp_ive_ms;
            const double other_ms = std::max(0.0, pre_ms - measured_ms);
            ALG_LOGI("[%s] aipp_pre setup=%.2fms bind=%.2fms copy=%.2fms copy_cpu=%.2fms "
                     "flush=%.2fms ive=%.2fms other=%.2fms reused=%d split=%d "
                     "external=%d ive_path=%d "
                     "bytes=%zu stride=%zu",
                     cfg_.name.c_str(), state.aipp_setup_ms, state.aipp_bind_ms, state.aipp_copy_ms,
                     state.aipp_copy_cpu_ms, state.aipp_flush_ms, state.aipp_ive_ms, other_ms,
                     state.aipp_staging_reused ? 1 : 0, state.aipp_split_planes ? 1 : 0,
                     state.aipp_external_zero_copy ? 1 : 0, state.aipp_ive_resize ? 1 : 0,
                     state.aipp_staging_bytes, state.aipp_staging_stride);
        }
    }
    return ALG_OK;
}

}  // namespace alg
