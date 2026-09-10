#include "backend/svp_acl/svp_acl_inferer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

#include "core/image_view.h"
#include "core/logger.h"

#ifdef ALG_SVP_DYNAMIC_AIPP
#include "libyuv/planar_functions.h"
#endif

namespace alg {
namespace {

struct SvpAclRuntime {
    int                refcount = 0;
    svp_acl_rt_context context = nullptr;
    bool               device_set = false;

    svp_acl_error Acquire() {
        if (refcount > 0) {
            ++refcount;
            return SVP_ACL_SUCCESS;
        }
        svp_acl_error ret = svp_acl_init(nullptr);
        if (ret != SVP_ACL_SUCCESS) {
            ALG_LOGE("svp_acl_init failed, ret=%d", ret);
            return ret;
        }
        ret = svp_acl_rt_set_device(0);
        if (ret != SVP_ACL_SUCCESS) {
            ALG_LOGE("svp_acl_rt_set_device failed, ret=%d", ret);
            (void)svp_acl_finalize();
            return ret;
        }
        device_set = true;
        ret = svp_acl_rt_create_context(&context, 0);
        if (ret != SVP_ACL_SUCCESS) {
            ALG_LOGE("svp_acl_rt_create_context failed, ret=%d", ret);
            (void)svp_acl_rt_reset_device(0);
            device_set = false;
            (void)svp_acl_finalize();
            return ret;
        }
        refcount = 1;
        return SVP_ACL_SUCCESS;
    }

    void Release() {
        if (refcount <= 0 || --refcount > 0) return;
        if (context) {
            (void)svp_acl_rt_destroy_context(context);
            context = nullptr;
        }
        if (device_set) {
            (void)svp_acl_rt_reset_device(0);
            device_set = false;
        }
        (void)svp_acl_finalize();
    }
};

SvpAclRuntime g_runtime;

bool ReadFile(const std::string& path, std::vector<uint8_t>* bytes) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const std::streamsize size = file.tellg();
    if (size <= 0) return false;
    bytes->resize(static_cast<size_t>(size));
    file.seekg(0);
    return static_cast<bool>(
        file.read(reinterpret_cast<char*>(bytes->data()), size));
}

DataType MapDtype(svp_acl_data_type type, bool* supported) {
    *supported = true;
    switch (type) {
        case SVP_ACL_UINT8:  return DataType::kU8;
        case SVP_ACL_INT8:   return DataType::kI8;
        case SVP_ACL_UINT16: return DataType::kU16;
        case SVP_ACL_INT16:  return DataType::kI16;
        case SVP_ACL_FLOAT16:return DataType::kF16;
        case SVP_ACL_FLOAT:  return DataType::kF32;
        case SVP_ACL_INT32:  return DataType::kI32;
        default:
            *supported = false;
            return DataType::kU8;
    }
}

bool IsAuxiliaryInput(const char* name) {
    if (!name) return false;
    return std::strcmp(name, "task_buf") == 0 ||
           std::strcmp(name, "work_buf") == 0;
}

void DestroyDataset(svp_acl_mdl_dataset*& dataset) {
    if (!dataset) return;
    const size_t count = svp_acl_mdl_get_dataset_num_buffers(dataset);
    for (size_t i = 0; i < count; ++i) {
        svp_acl_data_buffer* buffer =
            svp_acl_mdl_get_dataset_buffer(dataset, i);
        if (!buffer) continue;
        (void)svp_acl_destroy_data_buffer(buffer);
    }
    (void)svp_acl_mdl_destroy_dataset(dataset);
    dataset = nullptr;
}

std::string DimsString(const svp_acl_mdl_io_dims& dims) {
    std::string text;
    for (size_t i = 0; i < dims.dim_count; ++i) {
        if (i) text += "x";
        text += std::to_string(dims.dims[i]);
    }
    return text;
}

Status MakeTensorView(const svp_acl_mdl_io_dims& dims,
                      svp_acl_data_type acl_type,
                      void* address,
                      size_t size,
                      size_t stride,
                      TensorView* view) {
    bool supported = false;
    const DataType dtype = MapDtype(acl_type, &supported);
    if (!supported) {
        ALG_LOGE("svp_acl: unsupported tensor dtype=%d", static_cast<int>(acl_type));
        return ALG_E_BACKEND;
    }
    if (dims.dim_count > static_cast<size_t>(Shape::kMaxDims)) {
        ALG_LOGE("svp_acl: tensor '%s' has too many dims=%zu",
                 dims.name, dims.dim_count);
        return ALG_E_BACKEND;
    }
    const int bytes = BytesPerElement(dtype);
    /* ATC 输出可能按 16/32 字节对齐行（如 1x32x37 FLOAT 行跨 160B、密集行宽 148B）。
     * dense 时 row_stride=0（绝大多数）；非 dense 记录实际行字节跨度，
     * 由按行读取的后处理器（lprnet_rec 等）负责按 row_stride 定位。 */
    size_t row_stride = 0;
    if (dims.dim_count > 0) {
        const size_t dense_row =
            static_cast<size_t>(dims.dims[dims.dim_count - 1]) * bytes;
        if (stride != dense_row) {
            row_stride = stride;
            ALG_LOGI("svp_acl: tensor '%s' has padded row stride %zu (dense %zu)",
                     dims.name, stride, dense_row);
        }
    }
    view->name = dims.name;
    view->dtype = dtype;
    view->layout = dims.dim_count == 4 ? Layout::kNCHW : Layout::kND;
    view->shape.ndims = static_cast<int>(dims.dim_count);
    for (size_t i = 0; i < dims.dim_count; ++i)
        view->shape.dims[i] = static_cast<int>(dims.dims[i]);
    view->quant.scale = 1.0f;
    view->quant.zero_point = 0;
    view->quant.quantized = false;
    view->data = address;
    view->size_bytes = size;
    view->row_stride = row_stride;
    return ALG_OK;
}

}  // namespace

SvpAclInferer::~SvpAclInferer() { FreeAll(); }

void SvpAclInferer::FreeAll() {
#ifdef ALG_SVP_DYNAMIC_AIPP
    if (dynamic_aipp_) {
        (void)svp_acl_mdl_destroy_aipp(dynamic_aipp_);
        dynamic_aipp_ = nullptr;
    }
    dynamic_aipp_input_index_ = static_cast<size_t>(-1);
    image_input_capacity_ = 0;
    own_image_input_ = nullptr;
    image_input_preflushed_ = false;
    split_copy_path_reported_ = false;
#endif
    input_views_.clear();
    output_views_.clear();
    output_raw_indices_.clear();
    DestroyDataset(input_dataset_);
    DestroyDataset(output_dataset_);
    for (void* memory : input_allocations_)
        if (memory) (void)svp_acl_rt_free(memory);
    for (void* memory : output_allocations_)
        if (memory) (void)svp_acl_rt_free(memory);
    input_allocations_.clear();
    output_allocations_.clear();
    if (model_desc_) {
        (void)svp_acl_mdl_destroy_desc(model_desc_);
        model_desc_ = nullptr;
    }
    if (model_loaded_) {
        (void)svp_acl_mdl_unload(model_id_);
        model_loaded_ = false;
    }
    if (model_memory_) {
        (void)svp_acl_rt_free(model_memory_);
        model_memory_ = nullptr;
    }
    if (runtime_acquired_) {
        g_runtime.Release();
        runtime_acquired_ = false;
    }
    image_input_index_ = static_cast<size_t>(-1);
    model_size_ = 0;
    initialized_ = false;
}

Status SvpAclInferer::Load(const std::string& model_path) {
    if (initialized_) return ALG_E_INVALID_ARG;
    if (g_runtime.Acquire() != SVP_ACL_SUCCESS) return ALG_E_BACKEND;
    runtime_acquired_ = true;

    Status status = ALG_E_BACKEND;
    do {
        std::vector<uint8_t> model_file;
        if (!ReadFile(model_path, &model_file)) {
            ALG_LOGE("svp_acl: cannot read model '%s'", model_path.c_str());
            break;
        }
        model_size_ = model_file.size();
        svp_acl_error ret = svp_acl_rt_malloc_cached(
            &model_memory_, model_size_, SVP_ACL_MEM_MALLOC_HUGE_FIRST);
        if (ret != SVP_ACL_SUCCESS) {
            ALG_LOGE("svp_acl: alloc model memory failed, ret=%d", ret);
            break;
        }
        std::memcpy(model_memory_, model_file.data(), model_size_);
        ret = svp_acl_rt_mem_flush(model_memory_, model_size_);
        if (ret != SVP_ACL_SUCCESS) {
            ALG_LOGE("svp_acl: flush model memory failed, ret=%d", ret);
            break;
        }
        ret = svp_acl_mdl_load_from_mem(
            model_memory_, model_size_, &model_id_);
        if (ret != SVP_ACL_SUCCESS) {
            ALG_LOGE("svp_acl: load model failed, ret=%d", ret);
            break;
        }
        model_loaded_ = true;
        model_desc_ = svp_acl_mdl_create_desc();
        if (!model_desc_) {
            ALG_LOGE("svp_acl: create model desc failed");
            break;
        }
        ret = svp_acl_mdl_get_desc(model_desc_, model_id_);
        if (ret != SVP_ACL_SUCCESS) {
            ALG_LOGE("svp_acl: get model desc failed, ret=%d", ret);
            break;
        }

        input_dataset_ = svp_acl_mdl_create_dataset();
        output_dataset_ = svp_acl_mdl_create_dataset();
        if (!input_dataset_ || !output_dataset_) {
            ALG_LOGE("svp_acl: create dataset failed");
            break;
        }

        const size_t input_count = svp_acl_mdl_get_num_inputs(model_desc_);
        const size_t output_count = svp_acl_mdl_get_num_outputs(model_desc_);
        ALG_LOGI("svp_acl: model='%s' inputs=%zu outputs=%zu",
                 model_path.c_str(), input_count, output_count);
        if (input_count == 0 || output_count == 0) break;
        input_allocations_.assign(input_count, nullptr);
        output_allocations_.assign(output_count, nullptr);

#ifdef ALG_SVP_DYNAMIC_AIPP
        /* 动态 AIPP 模型会增加一个由运行时自动填写的辅助输入。先找出其
         * 关联关系，避免把 AIPP 参数 tensor 误判为第二个业务输入。 */
        for (size_t i = 0; i < input_count; ++i) {
            svp_acl_mdl_aipp_type aipp_type{};
            size_t attached_index = static_cast<size_t>(-1);
            ret = svp_acl_mdl_get_input_aipp_type(
                model_id_, i, &aipp_type, &attached_index);
            if (ret == SVP_ACL_SUCCESS &&
                aipp_type == SVP_ACL_DATA_WITH_DYNAMIC_AIPP) {
                image_input_index_ = i;
                dynamic_aipp_input_index_ = attached_index;
                break;
            }
        }
#endif

        bool io_ok = true;
        for (size_t i = 0; i < input_count; ++i) {
            svp_acl_mdl_io_dims dims{};
            ret = svp_acl_mdl_get_input_dims(model_desc_, i, &dims);
            if (ret != SVP_ACL_SUCCESS) { io_ok = false; break; }
            const size_t size =
                svp_acl_mdl_get_input_size_by_index(model_desc_, i);
            const size_t stride =
                svp_acl_mdl_get_input_default_stride(model_desc_, i);
            void* memory = nullptr;
            ret = svp_acl_rt_malloc_cached(
                &memory, size, SVP_ACL_MEM_MALLOC_NORMAL_ONLY);
            if (ret != SVP_ACL_SUCCESS) { io_ok = false; break; }
            bool runtime_filled_aipp = false;
#ifdef ALG_SVP_DYNAMIC_AIPP
            runtime_filled_aipp = i == dynamic_aipp_input_index_;
#endif
            if (!runtime_filled_aipp) std::memset(memory, 0, size);
            if (!runtime_filled_aipp &&
                svp_acl_rt_mem_flush(memory, size) != SVP_ACL_SUCCESS) {
                (void)svp_acl_rt_free(memory);
                io_ok = false;
                break;
            }
            svp_acl_data_buffer* buffer =
                svp_acl_create_data_buffer(memory, size, stride);
            if (!buffer ||
                svp_acl_mdl_add_dataset_buffer(input_dataset_, buffer) !=
                    SVP_ACL_SUCCESS) {
                if (buffer) (void)svp_acl_destroy_data_buffer(buffer);
                (void)svp_acl_rt_free(memory);
                io_ok = false;
                break;
            }
            input_allocations_[i] = memory;
            bool auxiliary = IsAuxiliaryInput(dims.name);
#ifdef ALG_SVP_DYNAMIC_AIPP
            auxiliary = auxiliary || i == dynamic_aipp_input_index_;
            if (i == image_input_index_) {
                image_input_capacity_ = size;
                own_image_input_ = memory;
            }
#endif
            ALG_LOGI("svp_acl: input[%zu] name='%s' shape=%s dtype=%d "
                     "size=%zu stride=%zu%s",
                     i, dims.name, DimsString(dims).c_str(),
                     static_cast<int>(
                         svp_acl_mdl_get_input_data_type(model_desc_, i)),
                     size, stride, auxiliary ? " auxiliary" : "");
            if (!auxiliary && i != image_input_index_) {
                if (image_input_index_ != static_cast<size_t>(-1)) {
                    ALG_LOGE("svp_acl: multiple business inputs are not supported");
                    io_ok = false;
                    break;
                }
                image_input_index_ = i;
            }
        }
        if (!io_ok || image_input_index_ == static_cast<size_t>(-1)) break;

#ifdef ALG_SVP_DYNAMIC_AIPP
        if (dynamic_aipp_input_index_ != static_cast<size_t>(-1)) {
            dynamic_aipp_ = svp_acl_mdl_create_aipp(1);
            if (!dynamic_aipp_) {
                ALG_LOGE("svp_acl: create dynamic AIPP object failed");
                break;
            }
        }
#endif

        for (size_t i = 0; i < output_count; ++i) {
            svp_acl_mdl_io_dims dims{};
            ret = svp_acl_mdl_get_output_dims(model_desc_, i, &dims);
            if (ret != SVP_ACL_SUCCESS) { io_ok = false; break; }
            const size_t size =
                svp_acl_mdl_get_output_size_by_index(model_desc_, i);
            const size_t stride =
                svp_acl_mdl_get_output_default_stride(model_desc_, i);
            void* memory = nullptr;
            ret = svp_acl_rt_malloc_cached(
                &memory, size, SVP_ACL_MEM_MALLOC_NORMAL_ONLY);
            if (ret != SVP_ACL_SUCCESS) { io_ok = false; break; }
            svp_acl_data_buffer* buffer =
                svp_acl_create_data_buffer(memory, size, stride);
            if (!buffer ||
                svp_acl_mdl_add_dataset_buffer(output_dataset_, buffer) !=
                    SVP_ACL_SUCCESS) {
                if (buffer) (void)svp_acl_destroy_data_buffer(buffer);
                (void)svp_acl_rt_free(memory);
                io_ok = false;
                break;
            }
            output_allocations_[i] = memory;
            ALG_LOGI("svp_acl: output[%zu] name='%s' shape=%s dtype=%d "
                     "size=%zu stride=%zu",
                     i, dims.name, DimsString(dims).c_str(),
                     static_cast<int>(
                         svp_acl_mdl_get_output_data_type(model_desc_, i)),
                     size, stride);
        }
        if (!io_ok) break;

        status = BuildViews();
        if (status != ALG_OK) break;
        initialized_ = true;
        status = ALG_OK;
    } while (false);

    if (status != ALG_OK) FreeAll();
    return status;
}

Status SvpAclInferer::BuildViews() {
    input_views_.clear();
    output_views_.clear();
    output_raw_indices_.clear();

    svp_acl_mdl_io_dims input_dims{};
    svp_acl_error ret = svp_acl_mdl_get_input_dims(
        model_desc_, image_input_index_, &input_dims);
    if (ret != SVP_ACL_SUCCESS) return ALG_E_BACKEND;
    svp_acl_data_buffer* input_buffer =
        svp_acl_mdl_get_dataset_buffer(input_dataset_, image_input_index_);
    TensorView input;
    Status status = MakeTensorView(
        input_dims,
        svp_acl_mdl_get_input_data_type(model_desc_, image_input_index_),
        svp_acl_get_data_buffer_addr(input_buffer),
        svp_acl_get_data_buffer_size(input_buffer),
        svp_acl_mdl_get_input_default_stride(
            model_desc_, image_input_index_),
        &input);
    if (status != ALG_OK) return status;
    input_views_.push_back(std::move(input));

    const size_t output_count = svp_acl_mdl_get_num_outputs(model_desc_);
    for (size_t i = 0; i < output_count; ++i) {
        svp_acl_mdl_io_dims dims{};
        ret = svp_acl_mdl_get_output_dims(model_desc_, i, &dims);
        if (ret != SVP_ACL_SUCCESS) return ALG_E_BACKEND;
        svp_acl_data_buffer* buffer =
            svp_acl_mdl_get_dataset_buffer(output_dataset_, i);
        TensorView output;
        status = MakeTensorView(
            dims, svp_acl_mdl_get_output_data_type(model_desc_, i),
            svp_acl_get_data_buffer_addr(buffer),
            svp_acl_get_data_buffer_size(buffer),
            svp_acl_mdl_get_output_default_stride(model_desc_, i),
            &output);
        if (status != ALG_OK) return status;
        output_views_.push_back(std::move(output));
        output_raw_indices_.push_back(i);
    }
    return ALG_OK;
}

Status SvpAclInferer::Forward() {
    if (!initialized_) return ALG_E_NOT_INITIALIZED;

    svp_acl_data_buffer* image_buffer =
        svp_acl_mdl_get_dataset_buffer(input_dataset_, image_input_index_);
    svp_acl_error ret = SVP_ACL_SUCCESS;
#ifdef ALG_SVP_DYNAMIC_AIPP
    if (!image_input_preflushed_)
#endif
    {
        ret = svp_acl_rt_mem_flush(
            svp_acl_get_data_buffer_addr(image_buffer),
            svp_acl_get_data_buffer_size(image_buffer));
        if (ret != SVP_ACL_SUCCESS) {
            ALG_LOGE("svp_acl: input flush failed, ret=%d", ret);
            return ALG_E_BACKEND;
        }
    }
#ifdef ALG_SVP_DYNAMIC_AIPP
    image_input_preflushed_ = false;
#endif

    ret = svp_acl_mdl_execute(model_id_, input_dataset_, output_dataset_);
    if (ret != SVP_ACL_SUCCESS) {
        ALG_LOGE("svp_acl: execute failed, ret=%d", ret);
        return ALG_E_BACKEND;
    }

    for (size_t raw_index : output_raw_indices_) {
        svp_acl_data_buffer* buffer =
            svp_acl_mdl_get_dataset_buffer(output_dataset_, raw_index);
        ret = svp_acl_rt_mem_invalidate(
            svp_acl_get_data_buffer_addr(buffer),
            svp_acl_get_data_buffer_size(buffer));
        if (ret != SVP_ACL_SUCCESS) {
            ALG_LOGE("svp_acl: output invalidate failed, ret=%d", ret);
            return ALG_E_BACKEND;
        }
    }
    return ALG_OK;
}

bool SvpAclInferer::SupportsDynamicAipp() const {
#ifdef ALG_SVP_DYNAMIC_AIPP
    return dynamic_aipp_ != nullptr;
#else
    return false;
#endif
}

#ifdef ALG_SVP_DYNAMIC_AIPP
Status SvpAclInferer::EnsureOwnImageInput() {
    if (own_image_input_) return ALG_OK;
    if (image_input_index_ == static_cast<size_t>(-1) ||
        image_input_capacity_ == 0 ||
        image_input_index_ >= input_allocations_.size()) {
        return ALG_E_BACKEND;
    }

    void* memory = nullptr;
    svp_acl_error ret = svp_acl_rt_malloc_cached(
        &memory, image_input_capacity_, SVP_ACL_MEM_MALLOC_NORMAL_ONLY);
    if (ret != SVP_ACL_SUCCESS) {
        ALG_LOGE("svp_acl: allocate private AIPP input failed, ret=%d", ret);
        return ALG_E_OOM;
    }
    std::memset(memory, 0, image_input_capacity_);
    ret = svp_acl_rt_mem_flush(memory, image_input_capacity_);
    if (ret != SVP_ACL_SUCCESS) {
        (void)svp_acl_rt_free(memory);
        return ALG_E_BACKEND;
    }
    own_image_input_ = memory;
    input_allocations_[image_input_index_] = memory;
    return ALG_OK;
}
#endif

Status SvpAclInferer::PrepareDynamicAipp(const AlgImage& image,
                                         const PreprocessConfig& cfg,
                                         PreprocessState& state,
                                         SharedInputStaging* shared_staging) {
#ifndef ALG_SVP_DYNAMIC_AIPP
    (void)image; (void)cfg; (void)state; (void)shared_staging;
    return ALG_E_BACKEND;
#else
    if (!initialized_ || !dynamic_aipp_) return ALG_E_NOT_INITIALIZED;
    if (image.width <= 0 || image.height <= 0 ||
        (image.width & 1) || (image.height & 1) ||
        (image.format != ALG_PIX_NV12 && image.format != ALG_PIX_NV21)) {
        return ALG_E_INVALID_ARG;
    }
    ImageView source;
    Status resolve_status = ResolveImageView(image, &source);
    if (resolve_status != ALG_OK) return resolve_status;
    if ((cfg.input_format == InputFormatPolicy::kNV12 && image.format != ALG_PIX_NV12) ||
        (cfg.input_format == InputFormatPolicy::kNV21 && image.format != ALG_PIX_NV21)) {
        return ALG_E_INVALID_ARG;
    }
    if (image.width > cfg.max_input_width || image.height > cfg.max_input_height) {
        ALG_LOGE("svp_acl: AIPP source %dx%d exceeds configured maximum %dx%d",
                 image.width, image.height, cfg.max_input_width, cfg.max_input_height);
        return ALG_E_PREPROCESS;
    }
    if (cfg.color == ColorOrder::kGray) {
        ALG_LOGE("svp_acl: dynamic AIPP YUV path does not emit gray model input");
        return ALG_E_PREPROCESS;
    }
    const bool graph_padding = cfg.aipp_output_width > 0;
    if (cfg.resize != ResizeMode::kStretch && !graph_padding) {
        /* CV610 AIPP 的常量 padding 固定为 0，无法复现模型要求的 114。
         * 非 stretch 模式需要在 OM 图首插入常量 Pad，并在配置中声明其布局。 */
        ALG_LOGE("svp_acl: letterbox AIPP requires aipp_output_size and "
                 "aipp_graph_padding");
        return ALG_E_PREPROCESS;
    }
    if (cfg.std[0] == 0.0f || cfg.std[1] == 0.0f || cfg.std[2] == 0.0f)
        return ALG_E_INVALID_ARG;

    int output_width = cfg.net_width;
    int output_height = cfg.net_height;
    float scale_ratio = 1.0f;
    int pad_left = 0;
    int pad_top = 0;
    if (cfg.resize != ResizeMode::kStretch) {
        switch (cfg.resize) {
            case ResizeMode::kLetterboxTL:
                scale_ratio = static_cast<float>(cfg.net_width) /
                              std::max(image.width, image.height);
                break;
            case ResizeMode::kLetterboxTLFit:
            case ResizeMode::kLetterboxCenter:
                scale_ratio = std::min(
                    static_cast<float>(cfg.net_width) / image.width,
                    static_cast<float>(cfg.net_height) / image.height);
                break;
            case ResizeMode::kStretch:
                break;
        }
        output_width = static_cast<int>(image.width * scale_ratio);
        output_height = static_cast<int>(image.height * scale_ratio);
        if (cfg.resize == ResizeMode::kLetterboxCenter) {
            pad_left = (cfg.net_width - output_width) / 2;
            pad_top = (cfg.net_height - output_height) / 2;
        }
        const int pad_right = cfg.net_width - output_width - pad_left;
        const int pad_bottom = cfg.net_height - output_height - pad_top;
        if (output_width != cfg.aipp_output_width ||
            output_height != cfg.aipp_output_height ||
            pad_left != cfg.graph_pad_left || pad_top != cfg.graph_pad_top ||
            pad_right != cfg.graph_pad_right || pad_bottom != cfg.graph_pad_bottom) {
            ALG_LOGE("svp_acl: source %dx%d produces AIPP %dx%d + pad [%d,%d,%d,%d], "
                     "but OM expects %dx%d + pad [%d,%d,%d,%d]",
                     image.width, image.height, output_width, output_height,
                     pad_left, pad_top, pad_right, pad_bottom,
                     cfg.aipp_output_width, cfg.aipp_output_height,
                     cfg.graph_pad_left, cfg.graph_pad_top,
                     cfg.graph_pad_right, cfg.graph_pad_bottom);
            return ALG_E_PREPROCESS;
        }
    } else if (graph_padding) {
        /* stretch 本身不需要补边。禁止将 graph padding 配到 stretch，避免
         * AIPP 输出尺寸与模型图声明不一致。 */
        ALG_LOGE("svp_acl: aipp_graph_padding is incompatible with stretch resize");
        return ALG_E_PREPROCESS;
    }

    /* 两种输入都以原图尺寸写入 ACL staging，再由 AIPP 缩放。分平面输入仅用
     * libyuv 合并平面，不在 CPU 上缩放，因此不同目标尺寸的检测模型也能共享。 */
    const bool copy_split_planes = source.uses_separate_planes;
    const int aipp_src_width = image.width;
    const int aipp_src_height = image.height;
    if (aipp_src_width <= 0 || aipp_src_height <= 0 ||
        (aipp_src_width & 1) || (aipp_src_height & 1)) {
        ALG_LOGE("svp_acl: AIPP YUV size must be positive and even, got %dx%d",
                 aipp_src_width, aipp_src_height);
        return ALG_E_PREPROCESS;
    }
    if (copy_split_planes && !split_copy_path_reported_ &&
        ALG_LOG_IS_ENABLED(alg::log::Level::Info)) {
        ALG_LOGI("svp_acl: split-plane %s %dx%d -> libyuv copy -> shared AIPP staging",
                 image.format == ALG_PIX_NV21 ? "NV21" : "NV12",
                 image.width, image.height);
        split_copy_path_reported_ = true;
    }

    svp_acl_error ret = svp_acl_mdl_set_aipp_src_image_size(
        dynamic_aipp_, aipp_src_width, aipp_src_height);
    if (ret != SVP_ACL_SUCCESS) return ALG_E_PREPROCESS;

    int8_t uv_swap = 0;
    const svp_acl_aipp_input_format format = image.format == ALG_PIX_NV21
        ? SVP_ACL_YVU420SP_U8 : SVP_ACL_YUV420SP_U8;
    ret = svp_acl_mdl_set_aipp_input_format(dynamic_aipp_, format);
    if (ret != SVP_ACL_SUCCESS && image.format == ALG_PIX_NV21) {
        /* 部分 CV610 工具链只接受 OM 声明的 YUV420SP 枚举。保持同一套 OM，
         * 把 NV21 当作 YUV420SP 读取，再由 rbuv_swap 交换 V/U。 */
        ret = svp_acl_mdl_set_aipp_input_format(dynamic_aipp_, SVP_ACL_YUV420SP_U8);
        uv_swap = 1;
    }
    if (ret != SVP_ACL_SUCCESS) return ALG_E_PREPROCESS;

    /* OpenCV NV12/NV21 默认的 BT.601 limited-range YUV -> full-range RGB。 */
    ret = svp_acl_mdl_set_aipp_csc_params(
        dynamic_aipp_, 1,
        1192, 0, 1634,
        1192, -400, -833,
        1192, 2066, 0,
        0, 0, 0, 16, 128, 128);
    if (ret != SVP_ACL_SUCCESS) return ALG_E_PREPROCESS;
    ret = svp_acl_mdl_set_aipp_rbuv_swap_switch(dynamic_aipp_, uv_swap);
    if (ret != SVP_ACL_SUCCESS) return ALG_E_PREPROCESS;
    ret = svp_acl_mdl_set_aipp_ax_swap_switch(dynamic_aipp_, 0);
    if (ret != SVP_ACL_SUCCESS) return ALG_E_PREPROCESS;

    ret = svp_acl_mdl_set_aipp_crop_params(dynamic_aipp_, 0, 0, 0, 0, 0, 0);
    if (ret != SVP_ACL_SUCCESS) return ALG_E_PREPROCESS;
    ret = svp_acl_mdl_set_aipp_padding_params(dynamic_aipp_, 0, 0, 0, 0, 0, 0);
    if (ret != SVP_ACL_SUCCESS) return ALG_E_PREPROCESS;

    const bool resize = image.width != output_width || image.height != output_height;
    ret = svp_acl_mdl_set_aipp_scf_params(
        dynamic_aipp_, resize ? 2 : 0,
        aipp_src_width, aipp_src_height, output_width, output_height, 0);
    if (ret != SVP_ACL_SUCCESS) return ALG_E_PREPROCESS;

    ret = svp_acl_mdl_set_aipp_dtc_pixel_mean(
        dynamic_aipp_, cfg.mean[0], cfg.mean[1], cfg.mean[2], 0.0f, 0);
    if (ret != SVP_ACL_SUCCESS) return ALG_E_PREPROCESS;
    ret = svp_acl_mdl_set_aipp_dtc_pixel_min(
        dynamic_aipp_, 0.0f, 0.0f, 0.0f, 0.0f, 0);
    if (ret != SVP_ACL_SUCCESS) return ALG_E_PREPROCESS;
    ret = svp_acl_mdl_set_aipp_pixel_var_reci(
        dynamic_aipp_, cfg.scale / cfg.std[0], cfg.scale / cfg.std[1],
        cfg.scale / cfg.std[2], 1.0f, 0);
    if (ret != SVP_ACL_SUCCESS) return ALG_E_PREPROCESS;

    ret = svp_acl_mdl_set_aipp_by_input_index(
        model_id_, input_dataset_, image_input_index_, dynamic_aipp_);
    if (ret != SVP_ACL_SUCCESS) {
        ALG_LOGE("svp_acl: attach dynamic AIPP failed, ret=%d", ret);
        return ALG_E_PREPROCESS;
    }

    const size_t dst_size = svp_acl_mdl_get_input_size_by_index(
        model_desc_, image_input_index_);
    const size_t dst_stride = svp_acl_mdl_get_input_default_stride(
        model_desc_, image_input_index_);
    if (dst_size == 0 || dst_size > image_input_capacity_ ||
        dst_stride < static_cast<size_t>(aipp_src_width) ||
        dst_stride > static_cast<size_t>(std::numeric_limits<int>::max())) {
        ALG_LOGE("svp_acl: invalid AIPP input buffer size=%zu stride=%zu capacity=%zu",
                 dst_size, dst_stride, image_input_capacity_);
        return ALG_E_PREPROCESS;
    }

    svp_acl_data_buffer* image_buffer =
        svp_acl_mdl_get_dataset_buffer(input_dataset_, image_input_index_);
    uint8_t* dst = static_cast<uint8_t*>(svp_acl_get_data_buffer_addr(image_buffer));
    size_t dst_y_bytes = 0;
    size_t dst_uv_bytes = 0;
    if (!CheckedPlaneByteSize(static_cast<int>(dst_stride), aipp_src_height,
                              &dst_y_bytes) ||
        !CheckedPlaneByteSize(static_cast<int>(dst_stride), aipp_src_height / 2,
                              &dst_uv_bytes) ||
        dst_y_bytes > std::numeric_limits<size_t>::max() - dst_uv_bytes) {
        return ALG_E_PREPROCESS;
    }
    const size_t needed_dst = dst_y_bytes + dst_uv_bytes;
    if (needed_dst > dst_size) return ALG_E_PREPROCESS;

    const bool can_share = shared_staging && shared_staging->flushed &&
        shared_staging->source_plane[0] == source.plane[0] &&
        shared_staging->source_plane[1] == source.plane[1] &&
        shared_staging->source_width == image.width &&
        shared_staging->source_height == image.height &&
        shared_staging->source_stride[0] == source.stride[0] &&
        shared_staging->source_stride[1] == source.stride[1] &&
        shared_staging->source_format == image.format &&
        shared_staging->source_data_len[0] == source.data_len[0] &&
        shared_staging->source_data_len[1] == source.data_len[1] &&
        shared_staging->source_uses_separate_planes == source.uses_separate_planes &&
        shared_staging->staged_width == aipp_src_width &&
        shared_staging->staged_height == aipp_src_height &&
        shared_staging->data && shared_staging->capacity >= dst_size &&
        shared_staging->data_size == needed_dst &&
        shared_staging->row_stride == dst_stride;

    if (can_share) {
        ret = svp_acl_update_data_buffer(image_buffer, shared_staging->data,
                                         dst_size, dst_stride);
        if (ret != SVP_ACL_SUCCESS) return ALG_E_PREPROCESS;
        input_views_[0].data = shared_staging->data;
        input_views_[0].size_bytes = dst_size;
        image_input_preflushed_ = true;

        /* 本 inferer 后续始终由同一静态 chain 顺序取得共享帧，可释放不再使用的
         * 私有整帧缓存。dataset buffer 不拥有地址，FreeAll 只释放 allocations_。 */
        if (own_image_input_ && own_image_input_ != shared_staging->data) {
            (void)svp_acl_rt_free(own_image_input_);
            input_allocations_[image_input_index_] = nullptr;
            own_image_input_ = nullptr;
            ALG_LOGI("svp_acl: switched to shared AIPP staging, released private input");
        }
    } else {
        Status own_status = EnsureOwnImageInput();
        if (own_status != ALG_OK) return own_status;
        dst = static_cast<uint8_t*>(own_image_input_);
        ret = svp_acl_update_data_buffer(image_buffer, dst, dst_size, dst_stride);
        if (ret != SVP_ACL_SUCCESS) return ALG_E_PREPROCESS;
        input_views_[0].data = dst;
        input_views_[0].size_bytes = dst_size;

        if (copy_split_planes) {
            uint8_t* dst_uv = dst + dst_y_bytes;
            const int libyuv_status = image.format == ALG_PIX_NV21
                ? libyuv::NV21Copy(
                      source.plane[0], source.stride[0],
                      source.plane[1], source.stride[1],
                      dst, static_cast<int>(dst_stride),
                      dst_uv, static_cast<int>(dst_stride),
                      image.width, image.height)
                : libyuv::NV12Copy(
                      source.plane[0], source.stride[0],
                      source.plane[1], source.stride[1],
                      dst, static_cast<int>(dst_stride),
                      dst_uv, static_cast<int>(dst_stride),
                      image.width, image.height);
            if (libyuv_status != 0) {
                ALG_LOGE("svp_acl: libyuv plane copy failed, ret=%d", libyuv_status);
                return ALG_E_PREPROCESS;
            }
        } else if (static_cast<size_t>(source.stride[0]) == dst_stride &&
                   static_cast<size_t>(source.stride[1]) == dst_stride) {
            std::memcpy(dst, source.plane[0], needed_dst);
        } else {
            for (int y = 0; y < image.height; ++y)
                std::memcpy(dst + static_cast<size_t>(y) * dst_stride,
                            source.plane[0] + static_cast<size_t>(y) * source.stride[0],
                            image.width);
            uint8_t* dst_uv = dst + dst_y_bytes;
            for (int y = 0; y < image.height / 2; ++y)
                std::memcpy(dst_uv + static_cast<size_t>(y) * dst_stride,
                            source.plane[1] + static_cast<size_t>(y) * source.stride[1],
                            image.width);
        }
        /* 只同步当前原图对应的 YUV 区域，不 flush 按最大输入预留的剩余空间。 */
        ret = svp_acl_rt_mem_flush(dst, needed_dst);
        if (ret != SVP_ACL_SUCCESS) {
            ALG_LOGE("svp_acl: staged AIPP input flush failed, ret=%d", ret);
            return ALG_E_BACKEND;
        }
        image_input_preflushed_ = true;

        if (shared_staging) {
            shared_staging->source_plane[0] = source.plane[0];
            shared_staging->source_plane[1] = source.plane[1];
            shared_staging->source_width = image.width;
            shared_staging->source_height = image.height;
            shared_staging->source_stride[0] = source.stride[0];
            shared_staging->source_stride[1] = source.stride[1];
            shared_staging->source_format = image.format;
            shared_staging->source_data_len[0] = source.data_len[0];
            shared_staging->source_data_len[1] = source.data_len[1];
            shared_staging->source_uses_separate_planes = source.uses_separate_planes;
            shared_staging->staged_width = aipp_src_width;
            shared_staging->staged_height = aipp_src_height;
            shared_staging->data = dst;
            shared_staging->capacity = image_input_capacity_;
            shared_staging->data_size = needed_dst;
            shared_staging->row_stride = dst_stride;
            shared_staging->flushed = true;
        }
    }

    state.scale_ratio = scale_ratio;
    state.pad_left = pad_left;
    state.pad_top = pad_top;
    state.original_width = image.width;
    state.original_height = image.height;
    return ALG_OK;
#endif
}

}  // namespace alg
