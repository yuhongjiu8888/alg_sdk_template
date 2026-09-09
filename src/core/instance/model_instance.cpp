#include "core/instance/model_instance.h"

#include <algorithm>

#include "backend/backend_factory.h"
#include "core/logger.h"
#include "core/preprocess/letterbox_preprocessor.h"
#include "core/registry/postprocessor_registry.h"

#ifdef ALG_PROFILE
#include <sys/time.h>
#endif

namespace alg {

#ifdef ALG_PROFILE
namespace {
inline double Ms(const timeval& a, const timeval& b) {
    return (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_usec - a.tv_usec) / 1000.0;
}
}  // namespace
#endif

ModelInstance::ModelInstance() = default;
ModelInstance::~ModelInstance() = default;

Status ModelInstance::Init(const ModelInstanceConfig& cfg) {
    cfg_ = cfg;

    inferer_ = MakeInferer();
    if (!inferer_) return ALG_E_BACKEND;
    Status s = inferer_->Load(cfg.model_path);
    if (s != ALG_OK) return s;
    if (inferer_->NumInputs() < 1 || inferer_->NumOutputs() < 1) {
        ALG_LOGE("[%s] inferer reports no I/O tensors", cfg.name.c_str());
        return ALG_E_BACKEND;
    }

    if (inferer_->SupportsDynamicAipp()) {
        if (cfg.pre.engine == PreprocessEngine::kOpenCV) {
            ALG_LOGE("[%s] dynamic-AIPP OM cannot use preprocess.engine=opencv",
                     cfg.name.c_str());
            return ALG_E_CONFIG;
        }
    } else {
        pre_.reset(new LetterboxPreprocessor());
        s = pre_->Configure(cfg.pre, inferer_->InputView(0));
        if (s != ALG_OK) return s;
    }

    post_ = PostprocessorRegistry::Instance().Create(cfg.post_type);
    if (!post_) {
        ALG_LOGE("[%s] postprocess type '%s' not registered", cfg.name.c_str(),
                 cfg.post_type.c_str());
        return ALG_E_MODEL_NOT_FOUND;
    }
    s = post_->Configure(*inferer_, cfg.post_params);
    if (s != ALG_OK) return s;

    initialized_ = true;
    ALG_LOGI("[%s] model instance ready (post_type=%s, inputs=%d, outputs=%d)",
             cfg.name.c_str(), cfg.post_type.c_str(),
             inferer_->NumInputs(), inferer_->NumOutputs());
    return ALG_OK;
}

Status ModelInstance::Run(const AlgImage& image, std::vector<Object>* out) {
    return RunImpl(image, out);
}

Status ModelInstance::RunImpl(const AlgImage& image,
                              std::vector<Object>* out) {
    if (!initialized_) return ALG_E_NOT_INITIALIZED;
    if (!out) return ALG_E_INVALID_ARG;
    out->clear();

#ifdef ALG_PROFILE
    timeval t0, t1, t2, t3;
    gettimeofday(&t0, nullptr);
#endif

    PreprocessState state;
    Status s;
    const bool try_aipp = cfg_.pre.engine != PreprocessEngine::kOpenCV &&
                          inferer_->SupportsDynamicAipp();
    if (try_aipp) {
        s = inferer_->PrepareDynamicAipp(image, cfg_.pre, state);
    } else if (cfg_.pre.engine == PreprocessEngine::kAipp) {
        ALG_LOGE("[%s] preprocess.engine=aipp but dynamic AIPP is unavailable",
                 cfg_.name.c_str());
        return ALG_E_PREPROCESS;
    } else {
        if (!pre_) return ALG_E_PREPROCESS;
        s = pre_->Apply(image, inferer_->InputView(0), state);
    }
    if (s != ALG_OK) return s;
#ifdef ALG_PROFILE
    gettimeofday(&t1, nullptr);
#endif

    s = inferer_->Forward();
    if (s != ALG_OK) return s;
#ifdef ALG_PROFILE
    gettimeofday(&t2, nullptr);
#endif

    s = post_->Apply(*inferer_, state, out);
    if (s != ALG_OK) return s;
#ifdef ALG_PROFILE
    gettimeofday(&t3, nullptr);
    ALG_LOGI("[%s] pre=%.2fms infer=%.2fms post=%.2fms objects=%zu",
             cfg_.name.c_str(), Ms(t0, t1), Ms(t1, t2), Ms(t2, t3), out->size());
#endif
    return ALG_OK;
}

}  // namespace alg
