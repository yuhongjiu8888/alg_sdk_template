#include "backend/mnn/mnn_inferer.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "core/logger.h"

namespace alg {

namespace {

DataType MapMnnDtype(halide_type_t type) {
    if (type.code == halide_type_float) {
        if (type.bits == 16) return DataType::kF16;
        if (type.bits == 32) return DataType::kF32;
    }
    if (type.code == halide_type_int) {
        if (type.bits == 8)  return DataType::kI8;
        if (type.bits == 16) return DataType::kI16;
        if (type.bits == 32) return DataType::kI32;
    }
    if (type.code == halide_type_uint) {
        if (type.bits == 8)  return DataType::kU8;
        if (type.bits == 16) return DataType::kU16;
    }
    return DataType::kF32;
}

TensorView BuildTensorView(const MNN::Tensor* t) {
    TensorView v;
    v.dtype  = MapMnnDtype(t->getType());

    auto dims = t->shape();
    v.shape.ndims = static_cast<int>(dims.size());
    for (int i = 0; i < v.shape.ndims && i < Shape::kMaxDims; ++i)
        v.shape.dims[i] = dims[i];

    /* Auto-detect NCHW vs NHWC for 4D tensors.
     * Convention: if dims[1] looks like channels (small, <= 64), treat as NCHW;
     * if dims[3] looks like channels, treat as NHWC. */
    if (v.shape.ndims == 4) {
        if (v.shape.dims[1] <= 64 && v.shape.dims[1] != v.shape.dims[2])
            v.layout = Layout::kNCHW;
        else if (v.shape.dims[3] <= 64 && v.shape.dims[3] != v.shape.dims[1])
            v.layout = Layout::kNHWC;
        else
            v.layout = Layout::kNCHW;
    } else {
        v.layout = Layout::kNCHW;
    }

    v.quant.scale      = 1.0f;
    v.quant.zero_point = 0;
    v.quant.quantized  = false;

    v.data       = t->host<void>();
    v.size_bytes = t->size();
    return v;
}

/* Collect output tensors from MNN session, sorted by spatial size descending.
 * YOLOX strides [8, 16, 32] map to feature maps from largest to smallest,
 * so sorting H*W descending ensures OutputView(0) = stride 8, etc. */
std::vector<TensorView> CollectOutputs(MNN::Interpreter* interp, MNN::Session* session) {
    auto output_map = interp->getSessionOutputAll(session);

    struct Entry { TensorView view; int spatial; };
    std::vector<Entry> entries;
    for (auto& kv : output_map) {
        MNN::Tensor* t = kv.second;
        if (!t || t->size() == 0) continue;
        TensorView v = BuildTensorView(t);
        v.name = kv.first;
        int h = (v.layout == Layout::kNHWC) ? v.shape.dims[1] : v.shape.dims[2];
        int w = (v.layout == Layout::kNHWC) ? v.shape.dims[2] : v.shape.dims[3];
        entries.push_back({v, h * w});
    }

    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.spatial > b.spatial; });

    std::vector<TensorView> result;
    for (auto& e : entries) result.push_back(e.view);
    return result;
}

}  // namespace

MnnInferer::MnnInferer() = default;
MnnInferer::~MnnInferer() { FreeAll(); }

void MnnInferer::FreeAll() {
    if (interpreter_) {
        MNN::Interpreter::destroy(interpreter_);
        interpreter_ = nullptr;
    }
    session_ = nullptr;
    initialized_ = false;
}

Status MnnInferer::Load(const std::string& model_path) {
    if (initialized_) return ALG_E_INVALID_ARG;

    interpreter_ = MNN::Interpreter::createFromFile(model_path.c_str());
    if (!interpreter_) {
        ALG_LOGE("MNN: failed to load model: %s", model_path.c_str());
        return ALG_E_BACKEND;
    }

    MNN::ScheduleConfig config;
    config.type = MNN_FORWARD_CPU;
    config.numThread = 1;

    session_ = interpreter_->createSession(config);
    if (!session_) {
        ALG_LOGE("MNN: failed to create session");
        FreeAll();
        return ALG_E_BACKEND;
    }

    Status s = BuildViews();
    if (s != ALG_OK) {
        FreeAll();
        return s;
    }

    initialized_ = true;
    ALG_LOGI("MNN: model loaded, %d inputs, %d outputs",
             NumInputs(), NumOutputs());
    return ALG_OK;
}

Status MnnInferer::BuildViews() {
    input_views_.clear();
    output_views_.clear();

    /* Inputs: collect all input tensor names, then query each. */
    auto input_names = interpreter_->getSessionInputAll(session_);
    for (auto& kv : input_names) {
        MNN::Tensor* t = kv.second;
        if (!t || t->size() == 0) continue;
        TensorView v = BuildTensorView(t);
        v.name = kv.first;
        input_views_.push_back(v);
    }

    /* Outputs: sorted by spatial size descending (matches strides order). */
    output_views_ = CollectOutputs(interpreter_, session_);

    if (input_views_.empty() || output_views_.empty()) {
        ALG_LOGE("MNN: no I/O tensors (in=%zu out=%zu)",
                 input_views_.size(), output_views_.size());
        return ALG_E_BACKEND;
    }

    for (size_t i = 0; i < output_views_.size(); ++i) {
        const auto& v = output_views_[i];
        ALG_LOGI("MNN: output[%zu] '%s' shape=(%d,%d,%d,%d) layout=%s dtype=%d",
                 i, v.name.c_str(),
                 v.shape.dims[0], v.shape.dims[1], v.shape.dims[2], v.shape.dims[3],
                 v.layout == Layout::kNHWC ? "NHWC" : "NCHW",
                 static_cast<int>(v.dtype));
    }
    return ALG_OK;
}

Status MnnInferer::Forward() {
    if (!initialized_) return ALG_E_NOT_INITIALIZED;

    /* Preprocessor has written into InputView(i).data, which points at
     * the MNN input tensor's host buffer. No copy needed. */

    auto ret = interpreter_->runSession(session_);
    if (ret != MNN::NO_ERROR) {
        ALG_LOGE("MNN: runSession failed, ret=%d", ret);
        return ALG_E_BACKEND;
    }

    /* Output tensor host pointers may change after runSession (MNN may
     * swap internal buffers). Re-collect and match by name to preserve
     * the sorted order established in BuildViews. */
    auto fresh = CollectOutputs(interpreter_, session_);
    for (size_t i = 0; i < output_views_.size() && i < fresh.size(); ++i) {
        if (output_views_[i].name == fresh[i].name) {
            output_views_[i].data = fresh[i].data;
        }
    }
    return ALG_OK;
}

}  // namespace alg
