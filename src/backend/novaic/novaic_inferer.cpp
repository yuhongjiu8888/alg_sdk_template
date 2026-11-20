/** NT98332 HDAL/GFX image preprocessing and AI2 inference. */
#include "backend/novaic/novaic_inferer.h"
#include <strings.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#include "core/logger.h"

extern "C" {
#include "vendor_gfx.h"
}
#include "vendor_ai_cpu/vendor_ai_cpu.h"

namespace alg {
namespace {

void Check(HD_RESULT result, const char* operation) {
    if (result != HD_OK)
        throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result));
}

void Cleanup(HD_RESULT result, const char* operation) {
    if (result != HD_OK)
        ALG_LOGE("[novaic] %s failed during cleanup: %d", operation, result);
}

std::mutex runtime_mutex;

void Sync(void* data, UINT32 size, HD_COMMON_MEM_DMA_DIR direction) {
    // HDAL cache maintenance operates on complete 32-byte lines.
    uintptr_t addr = reinterpret_cast<uintptr_t>(data);
    uintptr_t start = addr & ~uintptr_t(31);
    uint64_t end = (static_cast<uint64_t>(addr) + size + 31) & ~uint64_t(31);
    if (!data || !size || end > std::numeric_limits<uintptr_t>::max())
        throw std::runtime_error("invalid DMA cache range");
    Check(hd_common_mem_cache_sync(reinterpret_cast<void*>(start), end - start, direction), "hd_common_mem_cache_sync");
}

void Fill(const HD_GFX_IMG_BUF& image, HD_IRECT rect, int gray) {
    HD_GFX_DRAW_RECT fill{};
    fill.dst_img = image;
    fill.rect = rect;
    fill.type = HD_GFX_RECT_SOLID;
    // The models use limited-range YUV. AGE packs alpha, Y, U, V from the least significant byte.
    UINT32 y = 16 + (219 * gray + 127) / 255;
    fill.color = (128U << 24) | (128U << 16) | (y << 8) | 255U;
    Check(hd_gfx_draw_rect(&fill), "hd_gfx_draw_rect");
}

}  // namespace

// HDAL/GFX/AI2 are process-wide. Keep them alive until the last model has released its DMA buffers.
struct NovaicInferer::Runtime {
    bool common = false, gfx = false, ai = false;
    VENDOR_GFX_SYSCAPS caps{};

    void Init() {
        Check(hd_common_init(0), "hd_common_init");
        common = true;
        Check(hd_gfx_init(), "hd_gfx_init");
        gfx = true;
        Check(vendor_gfx_get_syscaps(&caps), "vendor_gfx_get_syscaps");
        if (!caps.image_align.w || !caps.image_align.h || caps.scl_rate <= 0)
            throw std::runtime_error("GFX reported invalid alignment or scale limits");
        Check(vendor_ai_cfg_set(VENDOR_AI_CFG_PLUGIN_ENGINE, vendor_ai_cpu1_get_engine()), "cfg_set(PLUGIN_ENGINE)");
        UINT32 scheduler = VENDOR_AI_PROC_SCHD_FAIR;
        Check(vendor_ai_cfg_set(VENDOR_AI_CFG_PROC_SCHD, &scheduler), "cfg_set(PROC_SCHD)");
        Check(vendor_ai_init(), "vendor_ai_init");
        ai = true;
    }

    ~Runtime() {
        if (ai)
            Cleanup(vendor_ai_uninit(), "vendor_ai_uninit");
        if (gfx)
            Cleanup(hd_gfx_uninit(), "hd_gfx_uninit");
        if (common)
            Cleanup(hd_common_uninit(), "hd_common_uninit");
    }

    bool Supports(int w, int h) const {
        return w > 0 && h > 0 && static_cast<unsigned>(w) >= caps.min_dim.w &&
            static_cast<unsigned>(h) >= caps.min_dim.h && static_cast<unsigned>(w) <= caps.max_dim.w &&
            static_cast<unsigned>(h) <= caps.max_dim.h && w % caps.image_align.w == 0 && h % caps.image_align.h == 0;
    }
};

void NovaicInferer::Buffer::Allocate(const char* name, uint64_t bytes) {
    if (!bytes || bytes > UINT32_MAX - 63U)
        throw std::runtime_error(std::string("invalid allocation size for ") + name);
    size = (bytes + 63) & ~uint64_t(63);
    char label[32];
    std::snprintf(label, sizeof(label), "%s", name);
    Check(hd_common_mem_alloc(label, &pa, &va, size, DDR_ID0), "hd_common_mem_alloc");
    if (!pa || !va)
        throw std::runtime_error(std::string("unmapped allocation for ") + name);
}

void NovaicInferer::Image::Allocate(const char* name, int width, int height) {
    UINT32 pitch = (static_cast<UINT32>(width) + 7U) & ~7U;
    uint64_t uv_offset = static_cast<uint64_t>(pitch) * height;
    buffer.Allocate(name, uv_offset + uv_offset / 2);
    gfx.dim = {static_cast<UINT32>(width), static_cast<UINT32>(height)};
    gfx.format = HD_VIDEO_PXLFMT_YUV420;
    gfx.ddr_id = DDR_ID0;
    gfx.lineoffset[0] = gfx.lineoffset[1] = pitch;
    gfx.p_phy_addr[0] = buffer.pa;
    gfx.p_phy_addr[1] = buffer.pa + uv_offset;
    Sync(buffer.va, buffer.size, HD_COMMON_MEM_DMA_TO_DEVICE);
}

void NovaicInferer::Buffer::Free() {
    if (va)
        Cleanup(hd_common_mem_free(pa, va), "hd_common_mem_free");
    *this = {};
}

NovaicInferer::~NovaicInferer() {
    FreeAll();
}

void NovaicInferer::FreeAll() {
    if (started_)
        Cleanup(vendor_ai_net_stop(id_), "vendor_ai_net_stop");
    if (opened_)
        Cleanup(vendor_ai_net_close(id_), "vendor_ai_net_close");
    if (acquired_)
        Cleanup(vendor_ai_release_id(id_), "vendor_ai_release_id");
    started_ = opened_ = acquired_ = prepared_ = false;
    input_views_.clear();
    output_views_.clear();
    outputs_.clear();
    output_paths_.clear();
    input_ = {};
    source_.reset();
    canvas_.buffer.Free();
    work_.Free();
    model_.Free();
    std::lock_guard<std::mutex> guard(runtime_mutex);
    runtime_.reset();
}

Status NovaicInferer::Load(const std::string& path) try {
    if (runtime_)
        return ALG_E_INVALID_ARG;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    std::streamoff length = file.tellg();
    if (length <= 0 || length > UINT32_MAX - 63U) {
        ALG_LOGE("[novaic] cannot read model or invalid size: %s", path.c_str());
        return ALG_E_IO;
    }
    {
        std::lock_guard<std::mutex> guard(runtime_mutex);
        static std::weak_ptr<Runtime> active_runtime;
        runtime_ = active_runtime.lock();
        if (!runtime_) {
            auto runtime = std::make_shared<Runtime>();
            runtime->Init();
            runtime_ = runtime;
            active_runtime = runtime;
        }
    }
    Check(vendor_ai_get_id(&id_), "vendor_ai_get_id");
    acquired_ = true;
    model_.Allocate("alg_model", length);
    file.seekg(0);
    if (!file.read(static_cast<char*>(model_.va), length))
        throw std::runtime_error("short model read");
    Sync(model_.va, model_.size, HD_COMMON_MEM_DMA_TO_DEVICE);
    VENDOR_AI_NET_CFG_MODEL model{};
    model.pa = model_.pa;
    model.va = reinterpret_cast<uintptr_t>(model_.va);
    model.size = length;
    Check(vendor_ai_net_set(id_, VENDOR_AI_NET_PARAM_CFG_MODEL, &model), "net_set(CFG_MODEL)");
    VENDOR_AI_NET_CFG_JOB_OPT jobs{};
    jobs.method = VENDOR_AI_NET_JOB_OPT_LINEAR;
    jobs.wait_ms = -1;
    Check(vendor_ai_net_set(id_, VENDOR_AI_NET_PARAM_CFG_JOB_OPT, &jobs), "net_set(CFG_JOB_OPT)");
    VENDOR_AI_NET_CFG_BUF_OPT buffers{};
    buffers.method = VENDOR_AI_NET_BUF_OPT_SHRINK;
    buffers.ddr_id = DDR_ID0;
    Check(vendor_ai_net_set(id_, VENDOR_AI_NET_PARAM_CFG_BUF_OPT, &buffers), "net_set(CFG_BUF_OPT)");
    Check(vendor_ai_net_open(id_), "vendor_ai_net_open");
    opened_ = true;
    VENDOR_AI_NET_INFO info{};
    Check(vendor_ai_net_get(id_, VENDOR_AI_NET_PARAM_INFO, &info), "net_get(INFO)");
    if (info.in_buf_cnt != 1 || !info.out_buf_cnt || info.out_buf_cnt > 256)
        throw std::runtime_error("expected one YUV input and 1..256 outputs");
    VENDOR_AI_NET_CFG_WORKBUF work{};
    Check(vendor_ai_net_get(id_, VENDOR_AI_NET_PARAM_CFG_WORKBUF, &work), "net_get(CFG_WORKBUF)");
    if (work.size) {
        work_.Allocate("alg_work", work.size);
        work.pa = work_.pa;
        work.va = reinterpret_cast<uintptr_t>(work_.va);
        work.size = work_.size;
        Check(vendor_ai_net_set(id_, VENDOR_AI_NET_PARAM_CFG_WORKBUF, &work), "net_set(CFG_WORKBUF)");
    }
    Check(vendor_ai_net_start(id_), "vendor_ai_net_start");
    started_ = true;
    Check(vendor_ai_net_get(id_, VENDOR_AI_NET_PARAM_IN_PATH_LIST, &input_path_), "net_get(IN_PATH_LIST)");
    Check(vendor_ai_net_get(id_, static_cast<VENDOR_AI_NET_PARAM_ID>(input_path_), &input_), "net_get(input)");
    if (input_.sign != MAKEFOURCC('A', 'B', 'U', 'F') || (input_.fmt & 0x7fffffffU) != HD_VIDEO_PXLFMT_YUV420 ||
        input_.batch_num > 1 || input_.time > 1)
        throw std::runtime_error("expected a single-frame YUV420 input");
    if (!input_.width || !input_.height) {
        // CNN25 YUV inputs may have dynamic dimensions. The CSC output on the same layer
        // carries the network's fixed RGB dimensions (OUT(layer, 0) in model metadata).
        VENDOR_AI_BUF rgb{};
        const auto csc_path = static_cast<VENDOR_AI_NET_PARAM_ID>(VENDOR_AI_NET_PARAM_OUT(input_path_, 0));
        Check(vendor_ai_net_get(id_, csc_path, &rgb), "net_get(CSC output)");
        if (rgb.sign != MAKEFOURCC('A', 'B', 'U', 'F') || rgb.channel != 3 ||
            (input_.width && input_.width != rgb.width) || (input_.height && input_.height != rgb.height))
            throw std::runtime_error("invalid CSC output for dynamic YUV input");
        input_.width = rgb.width;
        input_.height = rgb.height;
    }
    if (!runtime_->Supports(input_.width, input_.height) || ((input_.width | input_.height) & 1))
        throw std::runtime_error("model dimensions exceed GFX limits");
    canvas_.Allocate("alg_canvas", input_.width, input_.height);
    input_.channel = 2;
    input_.batch_num = input_.time = 1;
    input_.ddr_id = DDR_ID0;
    input_.pa = canvas_.buffer.pa;
    input_.va = reinterpret_cast<uintptr_t>(canvas_.buffer.va);
    input_.line_ofs = canvas_.gfx.lineoffset[0];
    input_.channel_ofs = input_.line_ofs * input_.height;
    input_.time_ofs = input_.batch_ofs = input_.size = input_.channel_ofs + input_.channel_ofs / 2;
    TensorView in;
    in.name = input_.name ? input_.name : "";
    in.dtype = DataType::kU8;
    in.layout = Layout::kND;
    in.shape.ndims = 2;
    in.shape.dims = {{static_cast<int>(input_.height + input_.height / 2), static_cast<int>(input_.width)}};
    in.row_stride = input_.line_ofs;
    in.size_bytes = input_.size;
    in.data = canvas_.buffer.va;
    input_views_.push_back(in);
    output_paths_.resize(info.out_buf_cnt);
    Check(vendor_ai_net_get(id_, VENDOR_AI_NET_PARAM_OUT_PATH_LIST, output_paths_.data()), "net_get(OUT_PATH_LIST)");
    output_views_.resize(info.out_buf_cnt);
    outputs_.resize(info.out_buf_cnt);
    for (size_t i = 0; i < outputs_.size(); ++i)
        ReadOutput(i, false);
    return ALG_OK;
} catch (const std::exception& e) {
    FreeAll();
    ALG_LOGE("[novaic] load %s: %s", path.c_str(), e.what());
    return ALG_E_BACKEND;
}

// The four CNN25 models emit WHCN signed fixed-point 16-bit features. Strip physical row/plane
// padding and expose the AI2 scale to the SDK's existing tensor decoder.
void NovaicInferer::ReadOutput(size_t index, bool copy) {
    VENDOR_AI_BUF out{};
    Check(vendor_ai_net_get(id_, static_cast<VENDOR_AI_NET_PARAM_ID>(output_paths_[index]), &out), "net_get(output)");
    if (out.sign != MAKEFOURCC('A', 'B', 'U', 'F') || HD_VIDEO_PXLFMT_FMT(out.fmt) != HD_VIDEO_PXLFMT_AI_SINT16 ||
        strncasecmp(out.layout, "whcn", 4) || !out.width || !out.height || !out.channel || out.batch_num > 1 ||
        out.time > 1 || !std::isfinite(out.scale_ratio) || out.scale_ratio <= 0)
        throw std::runtime_error("unsupported AI2 output descriptor at index " + std::to_string(index));
    const uint64_t row_bytes = uint64_t(out.width) * sizeof(int16_t);
    const uint64_t pitch = out.line_ofs ? out.line_ofs : row_bytes;
    if (pitch < row_bytes || pitch > UINT32_MAX || pitch * out.height > UINT32_MAX)
        throw std::runtime_error("invalid AI2 output line stride");
    const uint64_t plane = out.channel_ofs ? out.channel_ofs : pitch * out.height;
    if ((out.channel > 1 && plane < pitch * (out.height - 1) + row_bytes) ||
        plane * (out.channel - 1) + pitch * (out.height - 1) + row_bytes > out.size)
        throw std::runtime_error("AI2 output strides exceed buffer size");
    auto& view = output_views_[index];
    if (!copy) {
        auto& dense = outputs_[index];
        dense.resize(static_cast<size_t>(row_bytes * out.height * out.channel / sizeof(int16_t)));
        view.name = out.name ? out.name : "";
        view.dtype = DataType::kI16;
        view.shape.ndims = 4;
        view.shape.dims = {
            {1, static_cast<int>(out.channel), static_cast<int>(out.height), static_cast<int>(out.width)}};
        view.quant.quantized = true;
        view.size_bytes = dense.size() * sizeof(int16_t);
        view.data = dense.data();
    }
    view.quant.scale = std::ldexp(out.scale_ratio, -HD_VIDEO_PXLFMT_FRAC(out.fmt));
    if (!copy)
        return;
    if (static_cast<UINT32>(view.W()) != out.width || static_cast<UINT32>(view.H()) != out.height ||
        static_cast<UINT32>(view.C()) != out.channel)
        throw std::runtime_error("output shape changed after network start");
    auto* src = reinterpret_cast<const uint8_t*>(out.va);
    Sync(reinterpret_cast<void*>(out.va), out.size, HD_COMMON_MEM_DMA_FROM_DEVICE);
    if (pitch == row_bytes && plane == row_bytes * out.height) {
        std::memcpy(view.data, src, view.size_bytes);
    } else {
        auto* dst = static_cast<uint8_t*>(view.data);
        for (UINT32 c = 0; c < out.channel; ++c)
            for (UINT32 y = 0; y < out.height; ++y) {
                std::memcpy(dst, src + c * plane + y * pitch, row_bytes);
                dst += row_bytes;
            }
    }
}

Status NovaicInferer::Forward() try {
    if (!started_)
        return ALG_E_NOT_INITIALIZED;
    if (!prepared_)
        return ALG_E_PREPROCESS;
    prepared_ = false;
    Check(vendor_ai_net_set(id_, static_cast<VENDOR_AI_NET_PARAM_ID>(input_path_), &input_), "net_set(input)");
    Check(vendor_ai_net_proc(id_), "vendor_ai_net_proc");
    for (size_t i = 0; i < outputs_.size(); ++i)
        ReadOutput(i, true);
    return ALG_OK;
} catch (const std::exception& e) {
    ALG_LOGE("[novaic] forward network %u: %s", id_, e.what());
    return ALG_E_BACKEND;
}

Status NovaicInferer::ValidatePreprocessConfig(const PreprocessConfig& cfg) const {
    if (!started_)
        return ALG_E_NOT_INITIALIZED;
    if (cfg.net_width != static_cast<int>(input_.width) || cfg.net_height != static_cast<int>(input_.height) ||
        (cfg.engine != PreprocessEngine::kHardware && cfg.engine != PreprocessEngine::kAuto) ||
        cfg.layout != Layout::kNCHW || cfg.color != ColorOrder::kRGB || cfg.scale != 1.f ||
        cfg.resize == ResizeMode::kLetterboxTL)
        return ALG_E_CONFIG;
    for (int i = 0; i < 3; ++i)
        if (cfg.mean[i] != 0.f || cfg.std[i] != 1.f)
            return ALG_E_CONFIG;
    return ALG_OK;
}

HD_GFX_IMG_BUF NovaicInferer::Upload(const AlgImage& image, const ImageView& view, SharedInputStaging* shared) {
    const AlgPixelFormat order = (input_.fmt & 0x80000000U) == AI_PXLFMT_YUV420_NV12 ? ALG_PIX_NV12 : ALG_PIX_NV21;
    const UINT32 w = image.width, h = image.height;
    // Share canonical NV21 frames across stages; an NV12 model uses its own upload buffer.
    if (order != ALG_PIX_NV21)
        shared = nullptr;
    if (shared && shared->flushed && shared->source_format == image.format &&
        shared->source_plane[0] == view.plane[0] && shared->source_plane[1] == view.plane[1] &&
        shared->source_stride[0] == view.stride[0] && shared->source_stride[1] == view.stride[1] &&
        shared->staged_width == image.width && shared->staged_height == image.height) {
        HD_GFX_IMG_BUF gfx{};
        gfx.dim = {w, h};
        gfx.ddr_id = DDR_ID0;
        gfx.format = HD_VIDEO_PXLFMT_YUV420;
        gfx.p_phy_addr[0] = shared->device_phys_addr;
        gfx.p_phy_addr[1] = shared->device_phys_addr + shared->row_stride * h;
        gfx.lineoffset[0] = gfx.lineoffset[1] = shared->row_stride;
        return gfx;
    }
    if (!source_ || source_->gfx.dim.w != w || source_->gfx.dim.h != h) {
        std::unique_ptr<Image> next(new Image);
        next->Allocate("alg_frame", w, h);
        source_ = std::move(next);
    }
    auto* dst = static_cast<uint8_t*>(source_->buffer.va);
    const UINT32 pitch = source_->gfx.lineoffset[0];
    for (int p = 0; p < 2; ++p) {
        const uint8_t* src = view.plane[p];
        const UINT32 rows = p == 0 ? h : h / 2;
        for (UINT32 y = 0; y < rows; ++y) {
            if (p == 1 && image.format != order) {
                for (UINT32 x = 0; x < w; x += 2) {
                    dst[x] = src[x + 1];
                    dst[x + 1] = src[x];
                }
            } else {
                std::memcpy(dst, src, w);
            }
            dst += pitch;
            src += view.stride[p];
        }
    }
    Sync(source_->buffer.va, source_->buffer.size, HD_COMMON_MEM_DMA_TO_DEVICE);
    if (shared) {
        shared->source_plane[0] = view.plane[0];
        shared->source_plane[1] = view.plane[1];
        shared->source_stride[0] = view.stride[0];
        shared->source_stride[1] = view.stride[1];
        shared->source_format = image.format;
        shared->staged_width = image.width;
        shared->staged_height = image.height;
        shared->row_stride = pitch;
        shared->device_phys_addr = source_->buffer.pa;
        shared->flushed = true;
    }
    return source_->gfx;
}

Status NovaicInferer::PrepareHardwareInput(const AlgImage& image,
                                           const PreprocessConfig& cfg,
                                           PreprocessState& state,
                                           SharedInputStaging* shared) {
    CropGeometry frame;
    frame.crop_w = frame.ix2 = image.width;
    frame.crop_h = frame.iy2 = image.height;
    return Prepare(image, frame, cfg.pad_value, cfg, state, shared);
}

Status NovaicInferer::PrepareNv12Roi(const AlgImage& image,
                                     const CropGeometry& crop,
                                     const CropConfig& crop_cfg,
                                     const PreprocessConfig& cfg,
                                     PreprocessState& state,
                                     CropTransform* transform,
                                     SharedInputStaging* shared) {
    transform->offset_x = crop.offset_x;
    transform->offset_y = crop.offset_y;
    return Prepare(image, crop, crop_cfg.pad_value, cfg, state, shared);
}

Status NovaicInferer::Prepare(const AlgImage& image,
                              const CropGeometry& crop,
                              int crop_pad,
                              const PreprocessConfig& cfg,
                              PreprocessState& state,
                              SharedInputStaging* shared) try {
    prepared_ = false;
    if (!started_)
        return ALG_E_NOT_INITIALIZED;
    const Runtime& runtime = *runtime_;
    const int width = input_.width, height = input_.height;
    if (!IsYuv420Sp(image.format) || image.width > cfg.max_input_width || image.height > cfg.max_input_height ||
        image.width > static_cast<int>(runtime.caps.max_dim.w) ||
        image.height > static_cast<int>(runtime.caps.max_dim.h) ||
        (cfg.input_format == InputFormatPolicy::kNV12 && image.format != ALG_PIX_NV12) ||
        (cfg.input_format == InputFormatPolicy::kNV21 && image.format != ALG_PIX_NV21))
        return ALG_E_INVALID_ARG;
    ImageView source;
    Status status = ResolveImageView(image, &source);
    if (status != ALG_OK)
        return status;
    HD_IRECT active{0, 0, width, height};
    if (cfg.resize != ResizeMode::kStretch) {
        const double fit = std::min(double(width) / crop.crop_w, double(height) / crop.crop_h);
        active.w = std::max(2, static_cast<int>(crop.crop_w * fit + 1e-7) & ~1);
        active.h = std::max(2, static_cast<int>(crop.crop_h * fit + 1e-7) & ~1);
        if (cfg.resize == ResizeMode::kLetterboxCenter) {
            active.x = ((width - active.w) / 2) & ~1;
            active.y = ((height - active.h) / 2) & ~1;
        }
    }
    HD_GFX_SCALE scale{};
    scale.src_region = {crop.ix1, crop.iy1, crop.ix2 - crop.ix1, crop.iy2 - crop.iy1};
    const double sx = double(active.w) / crop.crop_w, sy = double(active.h) / crop.crop_h;
    auto even = [](double value) { return static_cast<int>(std::lround(value / 2)) * 2; };
    const int x = even((crop.ix1 - crop.offset_x) * sx), y = even((crop.iy1 - crop.offset_y) * sy);
    scale.dst_region = {active.x + x, active.y + y, even((crop.ix2 - crop.offset_x) * sx) - x,
                        even((crop.iy2 - crop.offset_y) * sy) - y};
    const HD_IRECT& src = scale.src_region;
    const HD_IRECT& dst = scale.dst_region;
    const int64_t rate = runtime.caps.scl_rate;
    if (!runtime.Supports(src.w, src.h) || !runtime.Supports(dst.w, dst.h) || src.w > dst.w * rate ||
        dst.w > src.w * rate || src.h > dst.h * rate || dst.h > src.h * rate) {
        ALG_LOGW("[novaic] GFX cannot scale %dx%d to %dx%d", src.w, src.h, dst.w, dst.h);
        return ALG_E_PREPROCESS;
    }
    scale.src_img = Upload(image, source, shared);
    scale.dst_img = canvas_.gfx;
    if (dst.w != width || dst.h != height) {
        Fill(canvas_.gfx, {0, 0, width, height}, cfg.pad_value);
        if (crop.padded && crop_pad != cfg.pad_value)
            Fill(canvas_.gfx, active, crop_pad);
    }
    Check(hd_gfx_scale(&scale), "hd_gfx_scale");
    state.original_width = crop.crop_w;
    state.original_height = crop.crop_h;
    state.scale_x = static_cast<float>(dst.w) / src.w;
    state.scale_y = static_cast<float>(dst.h) / src.h;
    state.scale_ratio = state.scale_x;
    state.pad_left = dst.x - std::lround((src.x - crop.offset_x) * state.scale_x);
    state.pad_top = dst.y - std::lround((src.y - crop.offset_y) * state.scale_y);
    prepared_ = true;
    return ALG_OK;
} catch (const std::exception& e) {
    ALG_LOGE("[novaic] preprocess network %u: %s", id_, e.what());
    return ALG_E_PREPROCESS;
}

}  // namespace alg
