#include "backend/ez/ez_inferer.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vx_ext_amd.h>
#include "core/logger.h"

namespace alg {
namespace {

bool Check(vx_status status, const char* operation) {
    if (status == VX_SUCCESS)
        return true;
    ALG_LOGE("[ez] %s failed: %d", operation, static_cast<int>(status));
    return false;
}

bool FinishOperation(vx_context context, vx_status status, const char* operation) {
    const bool operation_ok = Check(status, operation);
    // Drain even on an SDK error: caller-owned camera memory may be returned immediately after AlgRun.
    const bool finished = Check(vxFinish(context), "vxFinish");
    return operation_ok && finished;
}

bool AlignWidth(int width, int alignment, int* result) {
    if (width <= 0 || width > std::numeric_limits<int>::max() - (alignment - 1))
        return false;
    *result = (width + alignment - 1) / alignment * alignment;
    return true;
}

bool LogicalShape(vx_graph graph, std::string& name, Shape* shape) {
    std::vector<int> dims;
    net_hw_data_layout layout;
    if (!Check(GetNetBlobOriginDims(graph, name, dims), "GetNetBlobOriginDims") || dims.size() != 4 ||
        !Check(GetNetBlobDataLayout(graph, name, layout), "GetNetBlobDataLayout"))
        return false;
    shape->ndims = 4;
    // SDK origin dimensions are reversed and depend on hardware layout (API 2.29).
    if (layout == HW_PACK || layout == HW_NCHW) {
        shape->dims = {{dims[3], dims[2], dims[1], dims[0], 0, 0}};
    } else if (layout == HW_NCWH) {
        shape->dims = {{dims[3], dims[2], dims[0], dims[1], 0, 0}};
    } else if (layout == HW_NHWC) {
        shape->dims = {{dims[3], dims[0], dims[2], dims[1], 0, 0}};
    } else {
        ALG_LOGE("[ez] unsupported hardware layout: %d", static_cast<int>(layout));
        return false;
    }
    size_t elements = 1;
    for (int i = 0; i < 4; ++i) {
        if (shape->dims[i] <= 0 ||
            elements > static_cast<size_t>(std::numeric_limits<int>::max()) / shape->dims[i])
            return false;
        elements *= shape->dims[i];
    }
    return elements <= std::numeric_limits<size_t>::max() / sizeof(float);
}

bool IsSuffix(const std::string& text, const char* suffix) {
    const std::string ending(suffix);
    return text.size() >= ending.size() && text.compare(text.size() - ending.size(), ending.size(), ending) == 0;
}

}  // namespace

EzInferer::~EzInferer() { FreeAll(); }

void EzInferer::FreeImage(DeviceImage* image) {
    if (image->handle && context_)
        Check(FreeDeviceImageBuffer(context_, image->handle), "FreeDeviceImageBuffer");
    *image = DeviceImage{};
}

void EzInferer::FreeAll() {
    prepared_ = false;
    initialized_ = false;
    if (context_ && kernels_loaded_)
        Check(vxFinish(context_), "vxFinish(release)");
    if (graph_)
        Check(vxReleaseGraph(&graph_), "vxReleaseGraph");
    graph_ = nullptr;
    FreeImage(&scratch_);
    FreeImage(&canvas_);
    FreeImage(&source_);
    input_views_.clear();
    output_views_.clear();
    input_tensors_.clear();
    output_tensors_.clear();
    input_names_.clear();
    output_names_.clear();
    exported_outputs_.clear();
    output_placeholders_.clear();
    if (kernels_loaded_ && context_)
        Check(vxUnloadKernels(context_, "openvx-nn"), "vxUnloadKernels");
    kernels_loaded_ = false;
    if (context_)
        Check(vxReleaseContext(&context_), "vxReleaseContext");
    context_ = nullptr;
    net_width_ = net_height_ = 0;
}

Status EzInferer::Load(const std::string& model_path) {
    if (initialized_ || context_)
        return ALG_E_INVALID_ARG;
    if (!IsSuffix(model_path, ".ezb") && !IsSuffix(model_path, ".eezb")) {
        ALG_LOGE("[ez] model_path must name a .ezb topology (+ sibling .bin) or packed .eezb model");
        return ALG_E_CONFIG;
    }
    context_ = vxCreateContext();
    if (!context_ || !Check(vxGetStatus(reinterpret_cast<vx_reference>(context_)), "vxCreateContext")) {
        FreeAll();
        return ALG_E_BACKEND;
    }
    vx_int8* topology = nullptr;
    vx_int8* weights = nullptr;
    Status result = ALG_E_BACKEND;
    do {
        if (!Check(vxLoadKernels(context_, "openvx-nn"), "vxLoadKernels"))
            break;
        kernels_loaded_ = true;
        bool loaded = false;
        if (IsSuffix(model_path, ".eezb")) {
            loaded = Check(LoadNetModel(context_, model_path.c_str(), &topology, &weights), "LoadNetModel(packed)");
        } else {
            const std::string weights_path = model_path.substr(0, model_path.size() - 4) + ".bin";
            loaded = Check(LoadNetModel(context_, model_path.c_str(), true, &topology), "LoadNetModel(topology)") &&
                     Check(LoadNetModel(context_, weights_path.c_str(), true, &weights), "LoadNetModel(weights)");
        }
        if (!loaded)
            break;
        graph_ = CreateNetGraph(context_, reinterpret_cast<vx_uint32*>(topology), weights);
        if (!graph_ || !Check(vxGetStatus(reinterpret_cast<vx_reference>(graph_)), "CreateNetGraph") ||
            !Check(vxVerifyGraph(graph_), "vxVerifyGraph"))
            break;
        result = BuildViews();
        if (result != ALG_OK)
            break;
        initialized_ = true;
        ALG_LOGI("[ez] model loaded: %s, input=%dx%d outputs=%zu", model_path.c_str(), net_width_, net_height_,
                 output_views_.size());
    } while (false);
    // The official inference sample unloads model headers after graph verification.
    if (IsSuffix(model_path, ".eezb") && topology && weights) {
        Check(UnLoadNetModel(topology, weights), "UnLoadNetModel(packed)");
    } else {
        if (weights)
            Check(UnLoadNetModel(weights), "UnLoadNetModel(weights)");
        if (topology)
            Check(UnLoadNetModel(topology), "UnLoadNetModel(topology)");
    }
    if (!initialized_)
        FreeAll();
    return result;
}

Status EzInferer::BuildViews() {
    if (!Check(GetNetInputBlob(graph_, input_names_, input_tensors_), "GetNetInputBlob") ||
        !Check(GetNetOutputBlob(graph_, output_names_, output_tensors_), "GetNetOutputBlob"))
        return ALG_E_BACKEND;
    if (input_names_.size() != 1 || input_tensors_.size() != 1 || output_names_.empty() ||
        output_names_.size() != output_tensors_.size()) {
        ALG_LOGE("[ez] camera backend requires one image input and at least one output");
        return ALG_E_CONFIG;
    }
    TensorView input;
    input.name = input_names_[0];
    input.layout = Layout::kNCHW;
    if (!LogicalShape(graph_, input_names_[0], &input.shape))
        return ALG_E_BACKEND;
    if (input.N() != 1 || input.C() != 3) {
        ALG_LOGE("[ez] only batch=1, three-channel camera image models are supported");
        return ALG_E_CONFIG;
    }
    vx_enum input_type = 0;
    vx_size input_address = 0;
    vx_size input_size = 0;
    if (!Check(vxQueryTensor(input_tensors_[0], VX_TENSOR_DATA_TYPE, &input_type, sizeof(input_type)),
               "vxQueryTensor(input type)") ||
        !Check(vxQueryTensor(input_tensors_[0], VX_TENSOR_MEMORY_VIRT_ADDR, &input_address, sizeof(input_address)),
               "vxQueryTensor(input address)") ||
        !Check(vxQueryTensor(input_tensors_[0], VX_TENSOR_MEMORY_SIZE, &input_size, sizeof(input_size)),
               "vxQueryTensor(input size)"))
        return ALG_E_BACKEND;
    switch (input_type) {
        case VX_TYPE_UINT8:
            input.dtype = DataType::kU8;
            break;
        case VX_TYPE_INT8:
            input.dtype = DataType::kI8;
            break;
        case VX_TYPE_UINT16:
            input.dtype = DataType::kU16;
            break;
        case VX_TYPE_INT16:
            input.dtype = DataType::kI16;
            break;
        case VX_TYPE_FLOAT16:
            input.dtype = DataType::kF16;
            break;
        case VX_TYPE_FLOAT32:
            input.dtype = DataType::kF32;
            break;
        case VX_TYPE_INT32:
            input.dtype = DataType::kI32;
            break;
        default:
            ALG_LOGE("[ez] unsupported input tensor type: %d", static_cast<int>(input_type));
            return ALG_E_BACKEND;
    }
    if (input.dtype != DataType::kF16 && input.dtype != DataType::kF32) {
        // The official sample queries this vendor attribute as vx_size. On 826 it contains a signed
        // 32-bit fixed-point position; preserve that sign when converting it to the exponent.
        vx_size raw_position = 0;
        if (!Check(vxQueryTensor(input_tensors_[0], VX_TENSOR_FIXED_POINT_POSITION,
                                 &raw_position, sizeof(raw_position)), "vxQueryTensor(input fixed point)"))
            return ALG_E_BACKEND;
        const uint32_t position_bits = static_cast<uint32_t>(raw_position);
        int32_t fixed_position = 0;
        std::memcpy(&fixed_position, &position_bits, sizeof(fixed_position));
        if (fixed_position < -127 || fixed_position > 149) {
            ALG_LOGE("[ez] input fixed-point position cannot be represented as float32: %d", fixed_position);
            return ALG_E_BACKEND;
        }
        input.quant.quantized = true;
        input.quant.scale = std::ldexp(1.0f, -fixed_position);
        input.quant.zero_point = 0;
    }
    // This backend's InputView combines logical NCHW geometry with SDK-owned packed device memory.
    // It is for the hardware-image path only: Prepare imports image memory, never dense CHW bytes.
    input.data = reinterpret_cast<void*>(input_address);
    input.size_bytes = input_size;
    input_views_.push_back(input);
    net_width_ = input.W();
    net_height_ = input.H();
    exported_outputs_.resize(output_names_.size());
    output_placeholders_.resize(output_names_.size());
    for (size_t i = 0; i < output_names_.size(); ++i) {
        TensorView output;
        output.name = output_names_[i];
        output.dtype = DataType::kF32;
        output.layout = Layout::kNCHW;
        if (!LogicalShape(graph_, output_names_[i], &output.shape))
            return ALG_E_BACKEND;
        output_placeholders_[i].resize(static_cast<size_t>(output.shape.Numel()));
        output.data = output_placeholders_[i].data();
        output.size_bytes = output_placeholders_[i].size() * sizeof(float);
        output_views_.push_back(output);
        exported_outputs_[i] = net_blob_data{};
        exported_outputs_[i].layout = NCHW;
        exported_outputs_[i].need_quantize = false;
        ALG_LOGI("[ez] output[%zu] %s logical NCHW=(%d,%d,%d,%d), exported float32", i, output.name.c_str(),
                 output.N(), output.C(), output.H(), output.W());
    }
    return ALG_OK;
}

Status EzInferer::EnsureImage(DeviceImage* image, img_fmt format, int width, int height) {
    if (image->handle && image->format == format && image->width == width && image->height == height)
        return ALG_OK;
    FreeImage(image);
    image->handle = AllocDeviceImageBuffer(context_, format, width, height);
    if (!image->handle)
        return ALG_E_OOM;
    image->physical = vxGetDeviceMemoryPhysicalAddress(image->handle);
    image->data = reinterpret_cast<uint8_t*>(vxGetDeviceMemoryVirtualAddress(image->handle));
    image->size = vxGetDeviceMemorySize(image->handle);
    size_t required = 0;
    if (!CheckedPlaneByteSize(width, height, &required) ||
        required > std::numeric_limits<size_t>::max() / 4) {
        FreeImage(image);
        return ALG_E_INVALID_ARG;
    }
    required = (format == BGRA_HWC) ? required * 4 : required + required / 2;
    if (!image->physical || !image->data || image->size < required ||
        image->size - 1 > std::numeric_limits<vx_size>::max() - image->physical) {
        FreeImage(image);
        return ALG_E_BACKEND;
    }
    image->format = format;
    image->width = width;
    image->height = height;
    return ALG_OK;
}

Status EzInferer::ValidatePreprocessConfig(const PreprocessConfig& cfg) const {
    if (!initialized_)
        return ALG_E_NOT_INITIALIZED;
    if (cfg.engine != PreprocessEngine::kAuto && cfg.engine != PreprocessEngine::kHardware) {
        ALG_LOGE("[ez] preprocess.engine must be auto or hardware; AIPP belongs to HiSilicon only");
        return ALG_E_CONFIG;
    }
    if (cfg.net_width != net_width_ || cfg.net_height != net_height_ || cfg.layout != Layout::kNCHW ||
        cfg.color != ColorOrder::kBGR || cfg.scale != 1.0f) {
        ALG_LOGE("[ez] config must match model geometry, use color=bgr and identity normalization; "
                 "embed channel order/mean/std/scale in the converted model");
        return ALG_E_CONFIG;
    }
    for (int c = 0; c < 3; ++c) {
        if (cfg.mean[c] != 0.0f || cfg.std[c] != 1.0f) {
            ALG_LOGE("[ez] runtime mean/std must be identity; embed normalization in the converted model");
            return ALG_E_CONFIG;
        }
    }
    return ALG_OK;
}

Status EzInferer::ValidateConfig(const AlgImage& image, const PreprocessConfig& cfg) const {
    Status status = ValidatePreprocessConfig(cfg);
    if (status != ALG_OK)
        return status;
    if (!IsYuv420Sp(image.format)) {
        ALG_LOGE("[ez] camera input must be NV21 or NV12");
        return ALG_E_PREPROCESS;
    }
    if ((cfg.input_format == InputFormatPolicy::kNV21 && image.format != ALG_PIX_NV21) ||
        (cfg.input_format == InputFormatPolicy::kNV12 && image.format != ALG_PIX_NV12))
        return ALG_E_INVALID_ARG;
    if (image.width > cfg.max_input_width || image.height > cfg.max_input_height)
        return ALG_E_INVALID_ARG;
    return ALG_OK;
}

Status EzInferer::BindSource(const AlgImage& image, const ImageView& view, vx_size* y, vx_size* uv,
                              int* width, bool* external, bool* reused, SharedInputStaging* shared) {
    *external = false;
    *reused = false;
    // Width is the hardware row pitch. Crop bounds use image.width, excluding row padding.
    if (view.uses_separate_planes && view.phys_addr[0] && view.phys_addr[1] &&
        view.stride[0] == view.stride[1] && view.stride[0] % 32 == 0) {
        const uint64_t max_address = static_cast<uint64_t>(std::numeric_limits<vx_size>::max());
        size_t y_size = 0, uv_size = 0;
        if (!CheckedPlaneByteSize(view.stride[0], image.height, &y_size) ||
            !CheckedPlaneByteSize(view.stride[1], image.height / 2, &uv_size) ||
            view.phys_addr[0] > max_address || view.phys_addr[1] > max_address ||
            y_size - 1 > max_address - view.phys_addr[0] || uv_size - 1 > max_address - view.phys_addr[1])
            return ALG_E_INVALID_ARG;
        *y = static_cast<vx_size>(view.phys_addr[0]);
        *uv = static_cast<vx_size>(view.phys_addr[1]);
        *width = view.stride[0];
        *external = true;
        // Camera/VB owns cache coherency: callers must finish/flush producer writes before AlgRun.
        return ALG_OK;
    }
    int aligned_width = 0;
    if (!AlignWidth(image.width, 32, &aligned_width))
        return ALG_E_INVALID_ARG;
    const size_t y_size = static_cast<size_t>(aligned_width) * image.height;
    const size_t total_size = y_size + y_size / 2;
    if (shared && shared->data && shared->device_phys_addr && shared->flushed &&
        shared->source_plane[0] == view.plane[0] && shared->source_plane[1] == view.plane[1] &&
        shared->source_width == image.width && shared->source_height == image.height &&
        shared->source_stride[0] == view.stride[0] && shared->source_stride[1] == view.stride[1] &&
        shared->source_format == image.format && shared->source_data_len[0] == view.data_len[0] &&
        shared->source_data_len[1] == view.data_len[1] &&
        shared->source_uses_separate_planes == view.uses_separate_planes &&
        shared->staged_width == aligned_width && shared->staged_height == image.height &&
        shared->row_stride == static_cast<size_t>(aligned_width) && shared->data_size == total_size &&
        shared->capacity >= total_size) {
        const uint64_t max_address = static_cast<uint64_t>(std::numeric_limits<vx_size>::max());
        if (shared->device_phys_addr > max_address || total_size - 1 > max_address - shared->device_phys_addr)
            return ALG_E_INVALID_ARG;
        *y = static_cast<vx_size>(shared->device_phys_addr);
        *uv = *y + static_cast<vx_size>(y_size);
        *width = aligned_width;
        *reused = true;
        return ALG_OK;
    }
    // If this model owns the published allocation, invalidating it must invalidate the borrowed descriptor first.
    if (shared && shared->data == source_.data)
        shared->Reset();
    const img_fmt format = image.format == ALG_PIX_NV21 ? YVU420_SP : YUV420_SP;
    Status status = EnsureImage(&source_, format, aligned_width, image.height);
    if (status != ALG_OK)
        return status;
    for (int row = 0; row < image.height; ++row) {
        uint8_t* dst = source_.data + static_cast<size_t>(row) * aligned_width;
        std::memcpy(dst, view.plane[0] + static_cast<size_t>(row) * view.stride[0], image.width);
        std::memset(dst + image.width, 0, aligned_width - image.width);
    }
    for (int row = 0; row < image.height / 2; ++row) {
        uint8_t* dst = source_.data + y_size + static_cast<size_t>(row) * aligned_width;
        std::memcpy(dst, view.plane[1] + static_cast<size_t>(row) * view.stride[1], image.width);
        std::memset(dst + image.width, 128, aligned_width - image.width);
    }
    if (!Check(vxFlushDeviceMemoryCache(context_, source_.handle), "vxFlushDeviceMemoryCache(source)"))
        return ALG_E_BACKEND;
    if (total_size - 1 > std::numeric_limits<vx_size>::max() - source_.physical)
        return ALG_E_INVALID_ARG;
    *y = source_.physical;
    *uv = source_.physical + static_cast<vx_size>(y_size);
    *width = aligned_width;
    if (shared) {
        shared->source_plane[0] = view.plane[0];
        shared->source_plane[1] = view.plane[1];
        shared->source_width = image.width;
        shared->source_height = image.height;
        shared->source_stride[0] = view.stride[0];
        shared->source_stride[1] = view.stride[1];
        shared->source_format = image.format;
        shared->source_data_len[0] = view.data_len[0];
        shared->source_data_len[1] = view.data_len[1];
        shared->source_uses_separate_planes = view.uses_separate_planes;
        shared->staged_width = aligned_width;
        shared->staged_height = image.height;
        shared->data = source_.data;
        shared->device_phys_addr = source_.physical;
        shared->capacity = source_.size;
        shared->data_size = total_size;
        shared->row_stride = aligned_width;
        shared->flushed = true;
    }
    return ALG_OK;
}

Status EzInferer::PrepareHardwareInput(const AlgImage& image, const PreprocessConfig& cfg, PreprocessState& state,
                                     SharedInputStaging* shared) {
    CropGeometry geometry;
    geometry.ix2 = geometry.crop_w = image.width;
    geometry.iy2 = geometry.crop_h = image.height;
    geometry.inside = true;
    return Prepare(image, cfg, geometry, cfg.pad_value, state, shared);
}

Status EzInferer::PrepareNv12Roi(const AlgImage& image, const AlgBox& box, const CropConfig& crop,
                                 const PreprocessConfig& cfg, PreprocessState& state, CropTransform* transform,
                                 SharedInputStaging* shared) {
    prepared_ = false;
    CropGeometry geometry;
    if (!transform || !ComputeNv12CropGeometry(image, box, crop, &geometry))
        return ALG_E_INVALID_ARG;
    Status status = Prepare(image, cfg, geometry, geometry.padded ? crop.pad_value : cfg.pad_value, state, shared);
    if (status != ALG_OK)
        return status;
    transform->offset_x = geometry.offset_x;
    transform->offset_y = geometry.offset_y;
    transform->crop_w = geometry.crop_w;
    transform->crop_h = geometry.crop_h;
    return ALG_OK;
}

Status EzInferer::Prepare(const AlgImage& image, const PreprocessConfig& cfg, const CropGeometry& geometry,
                           int crop_pad_value, PreprocessState& state, SharedInputStaging* shared) {
    prepared_ = false;
    Status status = ValidateConfig(image, cfg);
    if (status != ALG_OK)
        return status;
    ImageView source_view;
    status = ResolveImageView(image, &source_view);
    if (status != ALG_OK)
        return status;
    if (geometry.crop_w <= 0 || geometry.crop_h <= 0 || crop_pad_value < 0 || crop_pad_value > 255)
        return ALG_E_INVALID_ARG;
    int resized_w = net_width_, resized_h = net_height_;
    int left = 0, top = 0;
    float scale = 1.0f;
    if (cfg.resize != ResizeMode::kStretch) {
        scale = cfg.resize == ResizeMode::kLetterboxTL
                    ? static_cast<float>(net_width_) / std::max(geometry.crop_w, geometry.crop_h)
                    : std::min(static_cast<float>(net_width_) / geometry.crop_w,
                               static_cast<float>(net_height_) / geometry.crop_h);
        resized_w = std::max(1, static_cast<int>(geometry.crop_w * scale));
        resized_h = std::max(1, static_cast<int>(geometry.crop_h * scale));
        if (resized_w > net_width_ || resized_h > net_height_) {
            ALG_LOGE("[ez] letterbox_tl exceeds network height; use letterbox_tl_fit for rectangular inputs");
            return ALG_E_CONFIG;
        }
        if (cfg.resize == ResizeMode::kLetterboxCenter) {
            left = (net_width_ - resized_w) / 2;
            top = (net_height_ - resized_h) / 2;
        }
    }
    const float scale_x = static_cast<float>(resized_w) / geometry.crop_w;
    const float scale_y = static_cast<float>(resized_h) / geometry.crop_h;
    int canvas_width = 0;
    if (!AlignWidth(net_width_, 16, &canvas_width))
        return ALG_E_INVALID_ARG;
    status = EnsureImage(&canvas_, BGRA_HWC, canvas_width, net_height_);
    if (status != ALG_OK)
        return status;
    // Only memset/copy runs on CPU; all pixel conversion, crop and scaling run on the image engine.
    const bool has_padding = top || left || resized_w != net_width_ || resized_h != net_height_;
    if (has_padding) {
        // Fill margins only; a stretch/full-canvas conversion needs no CPU memset.
        const size_t row_bytes = static_cast<size_t>(canvas_width) * 4;
        if (top)
            std::memset(canvas_.data, cfg.pad_value, static_cast<size_t>(top) * row_bytes);
        const int bottom_start = top + resized_h;
        if (bottom_start < net_height_)
            std::memset(canvas_.data + static_cast<size_t>(bottom_start) * row_bytes, cfg.pad_value,
                        static_cast<size_t>(net_height_ - bottom_start) * row_bytes);
        for (int row = top; row < bottom_start; ++row) {
            uint8_t* dst = canvas_.data + static_cast<size_t>(row) * row_bytes;
            if (left)
                std::memset(dst, cfg.pad_value, static_cast<size_t>(left) * 4);
            const int right_start = left + resized_w;
            if (right_start < net_width_)
                std::memset(dst + static_cast<size_t>(right_start) * 4, cfg.pad_value,
                            static_cast<size_t>(net_width_ - right_start) * 4);
        }
    }
    if (geometry.padded) {
        for (int row = top; row < top + resized_h; ++row)
            std::memset(canvas_.data + (static_cast<size_t>(row) * canvas_width + left) * 4,
                        crop_pad_value, static_cast<size_t>(resized_w) * 4);
    }
    if ((has_padding || geometry.padded) &&
        !Check(vxFlushDeviceMemoryCache(context_, canvas_.handle), "vxFlushDeviceMemoryCache(canvas fill)"))
        return ALG_E_BACKEND;
    bool external = false;
    bool reused = false;
    bool scratch_copy = false;
    if (geometry.ix2 > geometry.ix1 && geometry.iy2 > geometry.iy1) {
        vx_size physical_y = 0, physical_uv = 0;
        int source_width = 0;
        status = BindSource(image, source_view, &physical_y, &physical_uv, &source_width, &external, &reused, shared);
        if (status != ALG_OK)
            return status;
        const int x0 = left + static_cast<int>(std::lround((geometry.ix1 - geometry.offset_x) * scale_x));
        const int y0 = top + static_cast<int>(std::lround((geometry.iy1 - geometry.offset_y) * scale_y));
        const int x1 = left + static_cast<int>(std::lround((geometry.ix2 - geometry.offset_x) * scale_x));
        const int y1 = top + static_cast<int>(std::lround((geometry.iy2 - geometry.offset_y) * scale_y));
        const int output_w = x1 - x0, output_h = y1 - y0;
        if (x0 < 0 || y0 < 0 || x1 > net_width_ || y1 > net_height_ || output_w <= 0 || output_h <= 0)
            return ALG_E_PREPROCESS;
        DeviceImage* target = &canvas_;
        int target_x = x0, target_y = y0;
        if (x0 % 16 != 0) {
            // BGRA_HWC hardware crop_out_x must be 16-aligned (API 4.1).
            // Keep exact center/ROI coordinates by copying the small converted rectangle afterwards.
            int scratch_width = 0;
            if (!AlignWidth(output_w, 16, &scratch_width))
                return ALG_E_INVALID_ARG;
            status = EnsureImage(&scratch_, BGRA_HWC, scratch_width, output_h);
            if (status != ALG_OK)
                return status;
            target = &scratch_;
            target_x = target_y = 0;
            scratch_copy = true;
        }
        img_cvt_param parameter{};
        parameter.input_fmt = image.format == ALG_PIX_NV21 ? YVU420_SP : YUV420_SP;
        parameter.input_width = source_width;
        parameter.input_height = image.height;
        parameter.input_crop_x = geometry.ix1;
        parameter.input_crop_y = geometry.iy1;
        parameter.input_crop_w = geometry.ix2 - geometry.ix1;
        parameter.input_crop_h = geometry.iy2 - geometry.iy1;
        parameter.color_range = 0;
        parameter.output_fmt = BGRA_HWC;
        parameter.output_width = target->width;
        parameter.output_height = target->height;
        parameter.output_crop_x = target_x;
        parameter.output_crop_y = target_y;
        parameter.output_crop_w = output_w;
        parameter.output_crop_h = output_h;
        if (!FinishOperation(context_, ImageConvert(context_, &parameter, physical_y, physical_uv, target->handle),
                             "ImageConvert"))
            return ALG_E_BACKEND;
        if (scratch_copy) {
            if (!Check(vxFlushDeviceMemoryCache(context_, scratch_.handle), "vxFlushDeviceMemoryCache(scratch read)"))
                return ALG_E_BACKEND;
            for (int row = 0; row < output_h; ++row)
                std::memcpy(canvas_.data + (static_cast<size_t>(row + y0) * canvas_width + x0) * 4,
                            scratch_.data + static_cast<size_t>(row) * scratch_.width * 4,
                            static_cast<size_t>(output_w) * 4);
            if (!Check(vxFlushDeviceMemoryCache(context_, canvas_.handle), "vxFlushDeviceMemoryCache(canvas copy)"))
                return ALG_E_BACKEND;
        }
    }
    // crop_out_* in input_image is deprecated in this NDK. Import a same-size converted image;
    // explicit ImageConvert geometry above is authoritative. Model handles channel order and normalization.
    input_image imported{};
    imported.op = CVT;
    imported.fmt = BGRA_HWC;
    imported.width = canvas_.width;
    imported.height = net_height_;
    imported.u.cvt_param.crop_in_w = net_width_;
    imported.u.cvt_param.crop_in_h = net_height_;
    imported.u.cvt_param.color_range = 0;
    imported.phys_addr = canvas_.physical;
    if (!FinishOperation(context_, ImportNetInputDataFromImage(graph_, input_names_[0], &imported),
                         "ImportNetInputDataFromImage"))
        return ALG_E_BACKEND;
    state = PreprocessState{};
    state.original_width = geometry.crop_w;
    state.original_height = geometry.crop_h;
    state.scale_ratio = cfg.resize == ResizeMode::kStretch ? scale_x : scale;
    state.scale_x = scale_x;
    state.scale_y = scale_y;
    state.pad_left = left;
    state.pad_top = top;
    ALG_LOGD("[ez] image engine pre: source=%dx%d physical=%d source_copy=%d reused=%d model_copy=%d "
             "roi=%dx%d net=%dx%d",
             image.width, image.height, external ? 1 : 0, external || reused ? 0 : 1, reused ? 1 : 0, scratch_copy ? 1 : 0,
             geometry.crop_w, geometry.crop_h, net_width_, net_height_);
    prepared_ = true;
    return ALG_OK;
}

Status EzInferer::Forward() {
    if (!initialized_)
        return ALG_E_NOT_INITIALIZED;
    if (!prepared_)
        return ALG_E_PREPROCESS;
    prepared_ = false;
    if (!FinishOperation(context_, vxProcessGraph(graph_), "vxProcessGraph"))
        return ALG_E_BACKEND;
    for (size_t i = 0; i < output_names_.size(); ++i) {
        // Float export removes hardware packet/channel alignment and fixed-point scaling.
        // Export allocation is SDK-owned, cached by the graph and freed by vxReleaseGraph (API 2.34).
        net_blob_data& raw = exported_outputs_[i];
        raw.layout = NCHW;
        raw.need_quantize = false;
        if (!Check(ExportNetOutputRawData(graph_, output_names_[i], raw), "ExportNetOutputRawData"))
            return ALG_E_BACKEND;
        const TensorView& expected = output_views_[i];
        if (!raw.data || raw.type != DFLOAT32 || raw.layout != NCHW || raw.n != expected.N() ||
            raw.c != expected.C() || raw.h != expected.H() || raw.w != expected.W()) {
            ALG_LOGE("[ez] exported output[%zu] geometry/type differs from model metadata", i);
            return ALG_E_BACKEND;
        }
        output_views_[i].data = raw.data;
    }
    // Before the first inference, placeholders make tensor views valid for Configure().
    // Once SDK-owned dense outputs are available, retain only that single allocation.
    if (!output_placeholders_.empty())
        output_placeholders_.clear();
    return ALG_OK;
}

}  // namespace alg
