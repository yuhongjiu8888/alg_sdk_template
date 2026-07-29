#include "backend/svp_acl/svp_acl_inferer.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "core/logger.h"

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
        void* address = svp_acl_get_data_buffer_addr(buffer);
        if (address) (void)svp_acl_rt_free(address);
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
    if (dims.dim_count > 0) {
        const size_t dense_row =
            static_cast<size_t>(dims.dims[dims.dim_count - 1]) * bytes;
        if (stride != dense_row) {
            ALG_LOGE("svp_acl: tensor '%s' stride=%zu, dense_row=%zu; "
                     "current TensorView requires dense storage",
                     dims.name, stride, dense_row);
            return ALG_E_BACKEND;
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
    return ALG_OK;
}

}  // namespace

SvpAclInferer::~SvpAclInferer() { FreeAll(); }

void SvpAclInferer::FreeAll() {
    input_views_.clear();
    output_views_.clear();
    output_raw_indices_.clear();
    DestroyDataset(input_dataset_);
    DestroyDataset(output_dataset_);
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
            std::memset(memory, 0, size);
            if (svp_acl_rt_mem_flush(memory, size) != SVP_ACL_SUCCESS) {
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
            const bool auxiliary = IsAuxiliaryInput(dims.name);
            ALG_LOGI("svp_acl: input[%zu] name='%s' shape=%s dtype=%d "
                     "size=%zu stride=%zu%s",
                     i, dims.name, DimsString(dims).c_str(),
                     static_cast<int>(
                         svp_acl_mdl_get_input_data_type(model_desc_, i)),
                     size, stride, auxiliary ? " auxiliary" : "");
            if (!auxiliary) {
                if (image_input_index_ != static_cast<size_t>(-1)) {
                    ALG_LOGE("svp_acl: multiple business inputs are not supported");
                    io_ok = false;
                    break;
                }
                image_input_index_ = i;
            }
        }
        if (!io_ok || image_input_index_ == static_cast<size_t>(-1)) break;

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
    svp_acl_error ret = svp_acl_rt_mem_flush(
        svp_acl_get_data_buffer_addr(image_buffer),
        svp_acl_get_data_buffer_size(image_buffer));
    if (ret != SVP_ACL_SUCCESS) {
        ALG_LOGE("svp_acl: input flush failed, ret=%d", ret);
        return ALG_E_BACKEND;
    }

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

}  // namespace alg
