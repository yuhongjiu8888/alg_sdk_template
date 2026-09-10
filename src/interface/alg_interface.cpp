#include "alg_interface.h"

#include <new>
#include <string>
#include <vector>

#include "backend/backend_factory.h"
#include "core/config/config.h"
#include "core/logger.h"
#include "core/object.h"
#include "core/solution/chain_solution.h"

#define ALG_VERSION_STRING "alg_sdk.v2.1.0"

namespace {

struct Context {
    alg::ChainSolution solution;
    std::vector<alg::Object> objects_scratch;
};

}  // namespace

extern "C" {

AlgStatus AlgCreate(AlgHandle* handle, const char* config_json_path) {
    if (!handle || !config_json_path) return ALG_E_INVALID_ARG;

    alg::SolutionConfig cfg;
    std::string err;
    AlgStatus s = alg::LoadSolutionConfig(config_json_path, &cfg, &err);
    if (s != ALG_OK) return s;
    if (cfg.solution_type != "chain") {
        ALG_LOGE("only solution.type == 'chain' is supported, got '%s'",
                 cfg.solution_type.c_str());
        return ALG_E_CONFIG;
    }

    auto* ctx = new (std::nothrow) Context();
    if (!ctx) return ALG_E_OOM;

    s = ctx->solution.Init(cfg);
    if (s != ALG_OK) { delete ctx; return s; }

    *handle = reinterpret_cast<AlgHandle>(ctx);
    return ALG_OK;
}

AlgStatus AlgDestroy(AlgHandle handle) {
    if (!handle) return ALG_E_INVALID_ARG;
    delete reinterpret_cast<Context*>(handle);
    return ALG_OK;
}

AlgStatus AlgRun(AlgHandle handle, const AlgImage* image, AlgResult* result) {
    if (!handle || !image || !result) return ALG_E_INVALID_ARG;

    auto* ctx = reinterpret_cast<Context*>(handle);
    result->speed_limit_count = 0;
    result->sign_count = 0;
    result->license_plate_count = 0;
    AlgStatus s = ctx->solution.Run(*image, &ctx->objects_scratch);
    if (s != ALG_OK) return s;
    alg::FillAlgResult(ctx->objects_scratch, result);

    static long long frame_counter = 0;
    result->frame_id = frame_counter++;
    return ALG_OK;
}

AlgStatus AlgSetLogLevel(AlgLogLevel level) {
    if (level < ALG_LOG_OFF || level > ALG_LOG_DEBUG) return ALG_E_INVALID_ARG;
    alg::log::set_level(static_cast<alg::log::Level>(level));
    return ALG_OK;
}

const char* AlgVersion(void) {
    static std::string s = std::string(ALG_VERSION_STRING) + "+" + alg::BackendName();
    return s.c_str();
}

const char* AlgBackendName(void) { return alg::BackendName(); }

}  // extern "C"
