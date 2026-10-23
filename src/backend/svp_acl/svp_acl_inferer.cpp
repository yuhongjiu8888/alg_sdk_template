#include "backend/svp_acl/svp_acl_inferer.h"
#include <sys/time.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <limits>
#include <vector>

#ifdef ALG_SVP_IVE_PREPROCESS
#include <unistd.h>
#endif

#include "core/image_view.h"
#include "core/infer/aipp_scale.h"
#include "core/logger.h"

#ifdef ALG_SVP_LIBYUV_FALLBACK
/* libyuv 服务于 AIPP staging 的准备：分平面合并（NV12Copy/NV21Copy），
 * 以及 DV500 上替代 AIPP 缩放的 CPU 预缩放（NV12Scale + kFilterBox）。
 * 预编译库按架构放在 third_party/libyuv/ 下，由 CMake 按芯片选择。 */
#include "libyuv/planar_functions.h"
#include "libyuv/scale.h"
#endif

#ifdef ALG_SVP_IVE_PREPROCESS
/* CV610 随 IVE/ACL 发布的 sysmem 库导出 ot_mpi_sys_* 底层符号；公开的
 * ss_mpi_sys_mmz_* 接口是同一实现的兼容命名。随附 SVP 头文件不包含
 * ss_mpi_sys.h，因此在此按官方原型作私有声明。 */
extern "C" {
td_s32 ot_mpi_sys_mmz_alloc(
    td_phys_addr_t* phys_addr, td_void** virt_addr, const td_char* mmb, const td_char* zone, td_u32 len);
td_s32 ot_mpi_sys_mmz_free(td_phys_addr_t phys_addr, td_void* virt_addr);
}
#endif

namespace alg {
namespace {

/* DV500 CPU 预缩放：src -> dst 之前先做几次精确 1/2 折半，让最后一段
 * 落在 libyuv 的专用 NEON 路径上（1/3 之类的非整数比没有专用路径）。
 * 返回折半次数；0 表示直接一次缩到目标。 */
inline int PrescaleHalfSteps(int src_w, int src_h, int dst_w, int dst_h) {
    int hops = 0;
    /* %4：保证折半后的中间帧宽高仍为偶数，NV12 的 UV 平面才合法。 */
    while ((src_w % 4 == 0) && (src_h % 4 == 0) && (src_w / 2 > dst_w) && (src_h / 2 > dst_h)) {
        src_w /= 2;
        src_h /= 2;
        ++hops;
    }
    return hops;
}

/* SharedInputStaging 里的 source_* 是否描述的仍是当前这一帧。 */
inline bool SameSource(const SharedInputStaging& shared, const AlgImage& image, const ImageView& source) {
    return shared.source_plane[0] == source.plane[0] && shared.source_plane[1] == source.plane[1] &&
           shared.source_width == image.width && shared.source_height == image.height &&
           shared.source_stride[0] == source.stride[0] && shared.source_stride[1] == source.stride[1] &&
           shared.source_format == image.format && shared.source_data_len[0] == source.data_len[0] &&
           shared.source_data_len[1] == source.data_len[1] &&
           shared.source_uses_separate_planes == source.uses_separate_planes;
}

inline double ElapsedMs(const timeval& begin, const timeval& end) {
    return (end.tv_sec - begin.tv_sec) * 1000.0 + (end.tv_usec - begin.tv_usec) / 1000.0;
}

/* 当前线程累计 CPU 时间。与墙钟一起看，可区分“在算”还是“在等内存/被调度”。 */
inline double ThreadCpuMs() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

struct SvpAclRuntime {
    int refcount = 0;
    svp_acl_rt_context context = nullptr;
    bool device_set = false;

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
        if (refcount <= 0 || --refcount > 0)
            return;
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
    if (!file)
        return false;
    const std::streamsize size = file.tellg();
    if (size <= 0)
        return false;
    bytes->resize(static_cast<size_t>(size));
    file.seekg(0);
    return static_cast<bool>(file.read(reinterpret_cast<char*>(bytes->data()), size));
}

DataType MapDtype(svp_acl_data_type type, bool* supported) {
    *supported = true;
    switch (type) {
        case SVP_ACL_UINT8:
            return DataType::kU8;
        case SVP_ACL_INT8:
            return DataType::kI8;
        case SVP_ACL_UINT16:
            return DataType::kU16;
        case SVP_ACL_INT16:
            return DataType::kI16;
        case SVP_ACL_FLOAT16:
            return DataType::kF16;
        case SVP_ACL_FLOAT:
            return DataType::kF32;
        case SVP_ACL_INT32:
            return DataType::kI32;
        default:
            *supported = false;
            return DataType::kU8;
    }
}

bool IsAuxiliaryInput(const char* name) {
    if (!name)
        return false;
    return std::strcmp(name, "task_buf") == 0 || std::strcmp(name, "work_buf") == 0;
}

void DestroyDataset(svp_acl_mdl_dataset*& dataset) {
    if (!dataset)
        return;
    const size_t count = svp_acl_mdl_get_dataset_num_buffers(dataset);
    for (size_t i = 0; i < count; ++i) {
        svp_acl_data_buffer* buffer = svp_acl_mdl_get_dataset_buffer(dataset, i);
        if (!buffer)
            continue;
        (void)svp_acl_destroy_data_buffer(buffer);
    }
    (void)svp_acl_mdl_destroy_dataset(dataset);
    dataset = nullptr;
}

std::string DimsString(const svp_acl_mdl_io_dims& dims) {
    std::string text;
    for (size_t i = 0; i < dims.dim_count; ++i) {
        if (i)
            text += "x";
        text += std::to_string(dims.dims[i]);
    }
    return text;
}

#ifdef ALG_SVP_IVE_PREPROCESS
inline size_t AlignUp(size_t value, size_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}
#endif

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
        ALG_LOGE("svp_acl: tensor '%s' has too many dims=%zu", dims.name, dims.dim_count);
        return ALG_E_BACKEND;
    }
    const int bytes = BytesPerElement(dtype);
    /* ATC 输出可能按 16/32 字节对齐行（如 1x32x37 FLOAT 行跨 160B、密集行宽 148B）。
     * dense 时 row_stride=0（绝大多数）；非 dense 记录实际行字节跨度，
     * 由按行读取的后处理器（lprnet_rec 等）负责按 row_stride 定位。 */
    size_t row_stride = 0;
    if (dims.dim_count > 0) {
        const size_t dense_row = static_cast<size_t>(dims.dims[dims.dim_count - 1]) * bytes;
        if (stride != dense_row) {
            row_stride = stride;
            ALG_LOGI("svp_acl: tensor '%s' has padded row stride %zu (dense %zu)", dims.name, stride, dense_row);
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

SvpAclInferer::~SvpAclInferer() {
    FreeAll();
}

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
    cpu_prescale_reported_ = false;
#ifdef ALG_SVP_LIBYUV_FALLBACK
    FreePrescaleTmp();
#endif
#ifdef ALG_SVP_IVE_PREPROCESS
    ive_path_reported_ = false;
#endif
#endif
    input_views_.clear();
    output_views_.clear();
    output_raw_indices_.clear();
    DestroyDataset(input_dataset_);
    DestroyDataset(output_dataset_);
#ifdef ALG_SVP_IVE_PREPROCESS
    FreeIveBuffers();
#endif
    for (void* memory : input_allocations_)
        if (memory)
            (void)svp_acl_rt_free(memory);
    for (void* memory : output_allocations_)
        if (memory)
            (void)svp_acl_rt_free(memory);
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
    if (initialized_)
        return ALG_E_INVALID_ARG;
    if (g_runtime.Acquire() != SVP_ACL_SUCCESS)
        return ALG_E_BACKEND;
    runtime_acquired_ = true;

    Status status = ALG_E_BACKEND;
    do {
        std::vector<uint8_t> model_file;
        if (!ReadFile(model_path, &model_file)) {
            ALG_LOGE("svp_acl: cannot read model '%s'", model_path.c_str());
            break;
        }
        model_size_ = model_file.size();
        svp_acl_error ret = svp_acl_rt_malloc_cached(&model_memory_, model_size_, SVP_ACL_MEM_MALLOC_HUGE_FIRST);
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
        ret = svp_acl_mdl_load_from_mem(model_memory_, model_size_, &model_id_);
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
#ifdef ALG_SVP_CHIP
        ALG_LOGI("svp_acl: chip=%s model='%s' inputs=%zu outputs=%zu", ALG_SVP_CHIP, model_path.c_str(), input_count,
                 output_count);
#else
        ALG_LOGI("svp_acl: model='%s' inputs=%zu outputs=%zu", model_path.c_str(), input_count, output_count);
#endif
        if (input_count == 0 || output_count == 0)
            break;
        input_allocations_.assign(input_count, nullptr);
        output_allocations_.assign(output_count, nullptr);

#ifdef ALG_SVP_DYNAMIC_AIPP
        /* 动态 AIPP 模型会增加一个由运行时自动填写的辅助输入。先找出其
         * 关联关系，避免把 AIPP 参数 tensor 误判为第二个业务输入。 */
        for (size_t i = 0; i < input_count; ++i) {
            svp_acl_mdl_aipp_type aipp_type{};
            size_t attached_index = static_cast<size_t>(-1);
            ret = svp_acl_mdl_get_input_aipp_type(model_id_, i, &aipp_type, &attached_index);
            if (ret == SVP_ACL_SUCCESS && aipp_type == SVP_ACL_DATA_WITH_DYNAMIC_AIPP) {
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
            if (ret != SVP_ACL_SUCCESS) {
                io_ok = false;
                break;
            }
            const size_t size = svp_acl_mdl_get_input_size_by_index(model_desc_, i);
            const size_t stride = svp_acl_mdl_get_input_default_stride(model_desc_, i);
            void* memory = nullptr;
            ret = svp_acl_rt_malloc_cached(&memory, size, SVP_ACL_MEM_MALLOC_NORMAL_ONLY);
            if (ret != SVP_ACL_SUCCESS) {
                io_ok = false;
                break;
            }
            bool runtime_filled_aipp = false;
#ifdef ALG_SVP_DYNAMIC_AIPP
            runtime_filled_aipp = i == dynamic_aipp_input_index_;
#endif
            if (!runtime_filled_aipp)
                std::memset(memory, 0, size);
            if (!runtime_filled_aipp && svp_acl_rt_mem_flush(memory, size) != SVP_ACL_SUCCESS) {
                (void)svp_acl_rt_free(memory);
                io_ok = false;
                break;
            }
            svp_acl_data_buffer* buffer = svp_acl_create_data_buffer(memory, size, stride);
            if (!buffer || svp_acl_mdl_add_dataset_buffer(input_dataset_, buffer) != SVP_ACL_SUCCESS) {
                if (buffer)
                    (void)svp_acl_destroy_data_buffer(buffer);
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
                     static_cast<int>(svp_acl_mdl_get_input_data_type(model_desc_, i)), size, stride,
                     auxiliary ? " auxiliary" : "");
            if (!auxiliary && i != image_input_index_) {
                if (image_input_index_ != static_cast<size_t>(-1)) {
                    ALG_LOGE("svp_acl: multiple business inputs are not supported");
                    io_ok = false;
                    break;
                }
                image_input_index_ = i;
            }
        }
        if (!io_ok || image_input_index_ == static_cast<size_t>(-1))
            break;

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
            if (ret != SVP_ACL_SUCCESS) {
                io_ok = false;
                break;
            }
            const size_t size = svp_acl_mdl_get_output_size_by_index(model_desc_, i);
            const size_t stride = svp_acl_mdl_get_output_default_stride(model_desc_, i);
            void* memory = nullptr;
            ret = svp_acl_rt_malloc_cached(&memory, size, SVP_ACL_MEM_MALLOC_NORMAL_ONLY);
            if (ret != SVP_ACL_SUCCESS) {
                io_ok = false;
                break;
            }
            svp_acl_data_buffer* buffer = svp_acl_create_data_buffer(memory, size, stride);
            if (!buffer || svp_acl_mdl_add_dataset_buffer(output_dataset_, buffer) != SVP_ACL_SUCCESS) {
                if (buffer)
                    (void)svp_acl_destroy_data_buffer(buffer);
                (void)svp_acl_rt_free(memory);
                io_ok = false;
                break;
            }
            output_allocations_[i] = memory;
            ALG_LOGI("svp_acl: output[%zu] name='%s' shape=%s dtype=%d "
                     "size=%zu stride=%zu",
                     i, dims.name, DimsString(dims).c_str(),
                     static_cast<int>(svp_acl_mdl_get_output_data_type(model_desc_, i)), size, stride);
        }
        if (!io_ok)
            break;

        status = BuildViews();
        if (status != ALG_OK)
            break;
        initialized_ = true;
        status = ALG_OK;
    } while (false);

    if (status != ALG_OK)
        FreeAll();
    return status;
}

Status SvpAclInferer::BuildViews() {
    input_views_.clear();
    output_views_.clear();
    output_raw_indices_.clear();

    svp_acl_mdl_io_dims input_dims{};
    svp_acl_error ret = svp_acl_mdl_get_input_dims(model_desc_, image_input_index_, &input_dims);
    if (ret != SVP_ACL_SUCCESS)
        return ALG_E_BACKEND;
    svp_acl_data_buffer* input_buffer = svp_acl_mdl_get_dataset_buffer(input_dataset_, image_input_index_);
    TensorView input;
    Status status =
        MakeTensorView(input_dims, svp_acl_mdl_get_input_data_type(model_desc_, image_input_index_),
                       svp_acl_get_data_buffer_addr(input_buffer), svp_acl_get_data_buffer_size(input_buffer),
                       svp_acl_mdl_get_input_default_stride(model_desc_, image_input_index_), &input);
    if (status != ALG_OK)
        return status;
    input_views_.push_back(std::move(input));

    const size_t output_count = svp_acl_mdl_get_num_outputs(model_desc_);
    for (size_t i = 0; i < output_count; ++i) {
        svp_acl_mdl_io_dims dims{};
        ret = svp_acl_mdl_get_output_dims(model_desc_, i, &dims);
        if (ret != SVP_ACL_SUCCESS)
            return ALG_E_BACKEND;
        svp_acl_data_buffer* buffer = svp_acl_mdl_get_dataset_buffer(output_dataset_, i);
        TensorView output;
        status = MakeTensorView(dims, svp_acl_mdl_get_output_data_type(model_desc_, i),
                                svp_acl_get_data_buffer_addr(buffer), svp_acl_get_data_buffer_size(buffer),
                                svp_acl_mdl_get_output_default_stride(model_desc_, i), &output);
        if (status != ALG_OK)
            return status;
        output_views_.push_back(std::move(output));
        output_raw_indices_.push_back(i);
    }
    return ALG_OK;
}

Status SvpAclInferer::Forward() {
    if (!initialized_)
        return ALG_E_NOT_INITIALIZED;

    svp_acl_data_buffer* image_buffer = svp_acl_mdl_get_dataset_buffer(input_dataset_, image_input_index_);
    svp_acl_error ret = SVP_ACL_SUCCESS;
#ifdef ALG_SVP_DYNAMIC_AIPP
    if (!image_input_preflushed_)
#endif
    {
        ret = svp_acl_rt_mem_flush(svp_acl_get_data_buffer_addr(image_buffer),
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
        svp_acl_data_buffer* buffer = svp_acl_mdl_get_dataset_buffer(output_dataset_, raw_index);
        ret = svp_acl_rt_mem_invalidate(svp_acl_get_data_buffer_addr(buffer), svp_acl_get_data_buffer_size(buffer));
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
    if (own_image_input_)
        return ALG_OK;
    if (image_input_index_ == static_cast<size_t>(-1) || image_input_capacity_ == 0 ||
        image_input_index_ >= input_allocations_.size()) {
        return ALG_E_BACKEND;
    }

    void* memory = nullptr;
    svp_acl_error ret = svp_acl_rt_malloc_cached(&memory, image_input_capacity_, SVP_ACL_MEM_MALLOC_NORMAL_ONLY);
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

#ifdef ALG_SVP_LIBYUV_FALLBACK
Status SvpAclInferer::EnsurePrescaleTmp(int slot, size_t bytes) {
    if (slot < 0 || slot > 1 || bytes == 0)
        return ALG_E_INVALID_ARG;
    if (prescale_tmp_[slot] && prescale_tmp_capacity_[slot] >= bytes)
        return ALG_OK;
    if (prescale_tmp_[slot]) {
        std::free(prescale_tmp_[slot]);
        prescale_tmp_[slot] = nullptr;
        prescale_tmp_capacity_[slot] = 0;
    }

    /* 中间帧只被 libyuv 读写，不参与 ACL 绑定，普通对齐堆内存即可，
     * 不占 MMZ。 */
    void* memory = nullptr;
    if (posix_memalign(&memory, 64, bytes) != 0 || !memory) {
        ALG_LOGE("svp_acl: allocate CPU pre-scale temp (%zu bytes) failed", bytes);
        return ALG_E_OOM;
    }
    prescale_tmp_[slot] = static_cast<uint8_t*>(memory);
    prescale_tmp_capacity_[slot] = bytes;
    return ALG_OK;
}

void SvpAclInferer::FreePrescaleTmp() {
    for (int i = 0; i < 2; ++i) {
        if (prescale_tmp_[i]) {
            std::free(prescale_tmp_[i]);
            prescale_tmp_[i] = nullptr;
            prescale_tmp_capacity_[i] = 0;
        }
    }
}
#endif

#ifdef ALG_SVP_IVE_PREPROCESS
void SvpAclInferer::FreeIveBuffers() {
    if (ive_output_virt_) {
        const td_s32 ret = ot_mpi_sys_mmz_free(ive_output_phys_, ive_output_virt_);
        if (ret != TD_SUCCESS)
            ALG_LOGW("svp_acl: free IVE output MMZ failed, ret=%d", ret);
    }
    if (ive_assist_virt_) {
        const td_s32 ret = ot_mpi_sys_mmz_free(ive_assist_phys_, ive_assist_virt_);
        if (ret != TD_SUCCESS)
            ALG_LOGW("svp_acl: free IVE resize assist MMZ failed, ret=%d", ret);
    }
    ive_output_phys_ = 0;
    ive_output_virt_ = nullptr;
    ive_output_capacity_ = 0;
    ive_assist_phys_ = 0;
    ive_assist_virt_ = nullptr;
    ive_assist_capacity_ = 0;
}

Status SvpAclInferer::EnsureIveBuffers(int width, int height, size_t stride) {
    size_t y_bytes = 0;
    size_t uv_bytes = 0;
    if (width <= 0 || height <= 0 || (width & 1) || (height & 1) || stride < static_cast<size_t>(width) ||
        stride > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        !CheckedPlaneByteSize(static_cast<int>(stride), height, &y_bytes) ||
        !CheckedPlaneByteSize(static_cast<int>(stride), height / 2, &uv_bytes) ||
        y_bytes > std::numeric_limits<size_t>::max() - uv_bytes) {
        return ALG_E_PREPROCESS;
    }
    const size_t output_bytes = y_bytes + uv_bytes;
    if (output_bytes > static_cast<size_t>(std::numeric_limits<td_u32>::max()))
        return ALG_E_PREPROCESS;

    if (!ive_output_virt_ || ive_output_capacity_ < output_bytes) {
        if (ive_output_virt_) {
            (void)ot_mpi_sys_mmz_free(ive_output_phys_, ive_output_virt_);
            ive_output_phys_ = 0;
            ive_output_virt_ = nullptr;
            ive_output_capacity_ = 0;
        }
        td_void* virt = nullptr;
        const td_s32 ret =
            ot_mpi_sys_mmz_alloc(&ive_output_phys_, &virt, "alg_ive_aipp", nullptr, static_cast<td_u32>(output_bytes));
        if (ret != TD_SUCCESS || !virt) {
            ALG_LOGE("svp_acl: allocate IVE output MMZ (%zu bytes) failed, ret=%d", output_bytes, ret);
            ive_output_phys_ = 0;
            return ALG_E_OOM;
        }
        ive_output_virt_ = virt;
        ive_output_capacity_ = output_bytes;
    }

    /* CV610 文档：resize 辅助内存 = 12 + sum(component * 64)。
     * 单张 YUV420SP 有 Y/UV 两个分量。 */
    constexpr size_t kResizeAssistBytes = 12 + 2 * 64;
    const size_t assist_bytes = kResizeAssistBytes;
    if (!ive_assist_virt_) {
        td_void* virt = nullptr;
        const td_s32 ret = ot_mpi_sys_mmz_alloc(&ive_assist_phys_, &virt, "alg_ive_resize", nullptr,
                                                static_cast<td_u32>(assist_bytes));
        if (ret != TD_SUCCESS || !virt) {
            ALG_LOGE("svp_acl: allocate IVE resize assist MMZ failed, ret=%d", ret);
            ive_assist_phys_ = 0;
            return ALG_E_OOM;
        }
        ive_assist_virt_ = virt;
        ive_assist_capacity_ = assist_bytes;
    }
    return ALG_OK;
}
#endif
#endif

Status SvpAclInferer::PrepareDynamicAipp(const AlgImage& image,
                                         const PreprocessConfig& cfg,
                                         PreprocessState& state,
                                         SharedInputStaging* shared_staging) {
#ifndef ALG_SVP_DYNAMIC_AIPP
    (void)image;
    (void)cfg;
    (void)state;
    (void)shared_staging;
    return ALG_E_BACKEND;
#else
    if (!initialized_ || !dynamic_aipp_)
        return ALG_E_NOT_INITIALIZED;
    if (image.width <= 0 || image.height <= 0 || (image.width & 1) || (image.height & 1) ||
        (image.format != ALG_PIX_NV12 && image.format != ALG_PIX_NV21)) {
        return ALG_E_INVALID_ARG;
    }
    ImageView source;
    Status resolve_status = ResolveImageView(image, &source);
    if (resolve_status != ALG_OK)
        return resolve_status;
    if ((cfg.input_format == InputFormatPolicy::kNV12 && image.format != ALG_PIX_NV12) ||
        (cfg.input_format == InputFormatPolicy::kNV21 && image.format != ALG_PIX_NV21)) {
        return ALG_E_INVALID_ARG;
    }
    if (image.width > cfg.max_input_width || image.height > cfg.max_input_height) {
        ALG_LOGE("svp_acl: AIPP source %dx%d exceeds configured maximum %dx%d", image.width, image.height,
                 cfg.max_input_width, cfg.max_input_height);
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
                scale_ratio = static_cast<float>(cfg.net_width) / std::max(image.width, image.height);
                break;
            case ResizeMode::kLetterboxTLFit:
            case ResizeMode::kLetterboxCenter:
                scale_ratio = std::min(static_cast<float>(cfg.net_width) / image.width,
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
        if (output_width != cfg.aipp_output_width || output_height != cfg.aipp_output_height ||
            pad_left != cfg.graph_pad_left || pad_top != cfg.graph_pad_top || pad_right != cfg.graph_pad_right ||
            pad_bottom != cfg.graph_pad_bottom) {
            ALG_LOGE("svp_acl: source %dx%d produces AIPP %dx%d + pad [%d,%d,%d,%d], "
                     "but OM expects %dx%d + pad [%d,%d,%d,%d]",
                     image.width, image.height, output_width, output_height, pad_left, pad_top, pad_right, pad_bottom,
                     cfg.aipp_output_width, cfg.aipp_output_height, cfg.graph_pad_left, cfg.graph_pad_top,
                     cfg.graph_pad_right, cfg.graph_pad_bottom);
            return ALG_E_PREPROCESS;
        }
    } else if (graph_padding) {
        /* stretch 本身不需要补边。禁止将 graph padding 配到 stretch，避免
         * AIPP 输出尺寸与模型图声明不一致。 */
        ALG_LOGE("svp_acl: aipp_graph_padding is incompatible with stretch resize");
        return ALG_E_PREPROCESS;
    }

    /* 连续 MMZ/VB 映射直接绑定。分平面源若同时提供物理地址，则由 IVE
     * 缩放到 AIPP 有效区并输出为一块连续 MMZ；普通测试内存仍兼容 libyuv
     * 合并，后续缩放交给 AIPP。 */
    const bool copy_split_planes = source.uses_separate_planes;
    const bool profile = ALG_LOG_IS_ENABLED(alg::log::Level::Info);
    const bool has_any_phys = source.phys_addr[0] != 0 || source.phys_addr[1] != 0;
    if (copy_split_planes && has_any_phys && (source.phys_addr[0] == 0 || source.phys_addr[1] == 0)) {
        ALG_LOGE("svp_acl: split-plane IVE input requires both Y and UV/VU physical addresses");
        return ALG_E_INVALID_ARG;
    }
#ifdef ALG_SVP_IVE_PREPROCESS
    const bool use_ive_resize = copy_split_planes && has_any_phys;
#else
    const bool use_ive_resize = false;
#endif

    /* DV500 没有 IVE，而它的 AIPP 只要参与缩放（scf_switch=2 BILINEAR）就固定多出
     * ~128ms/帧（与源尺寸和缩放比无关）。所以这里改由 SDK 用 libyuv 先把整帧缩到
     * aipp_output_size，让 AIPP 只做色域转换（scf_switch=0）。
     *
     * 两条路：
     *   ALG_AIPP_SCALE_AUTO     —— 源尺寸不符时 SDK 内部预缩放；
     *   ALG_AIPP_SCALE_EXTERNAL —— 缩放由调用方（VPSS 通道）完成，尺寸不符直接报错，
     *                              不降级，避免静默掉回那 128ms 的路径。
     * cv610 不参与：它的 IVE 路径照旧，新逻辑被 ALG_SVP_CHIP_DV500 隔离。 */
    bool cpu_prescale = false;
#ifdef ALG_SVP_CHIP_DV500
    if (!use_ive_resize && (image.width != output_width || image.height != output_height)) {
        if (alg::aipp_scale::mode() == alg::aipp_scale::Mode::kExternal) {
            ALG_LOGE("svp_acl: aipp_scale=external but source is %dx%d, expected %dx%d "
                     "(VPSS channel not configured for aipp_output_size?)",
                     image.width, image.height, output_width, output_height);
            return ALG_E_PREPROCESS;
        }
#ifndef ALG_SVP_LIBYUV_FALLBACK
        ALG_LOGE("svp_acl: dv500 needs CPU pre-scale but this build has no libyuv "
                 "(ALG_SVP_LIBYUV_FALLBACK=OFF); rebuild with libyuv or use "
                 "AlgSetAippScaleMode(ALG_AIPP_SCALE_EXTERNAL)");
        return ALG_E_PREPROCESS;
#else
        cpu_prescale = true;
#endif
    }
#endif

    const int aipp_src_width = (use_ive_resize || cpu_prescale) ? output_width : image.width;
    const int aipp_src_height = (use_ive_resize || cpu_prescale) ? output_height : image.height;
    if (aipp_src_width <= 0 || aipp_src_height <= 0 || (aipp_src_width & 1) || (aipp_src_height & 1)) {
        ALG_LOGE("svp_acl: AIPP YUV size must be positive and even, got %dx%d", aipp_src_width, aipp_src_height);
        return ALG_E_PREPROCESS;
    }
    if (copy_split_planes && !use_ive_resize && !cpu_prescale && !split_copy_path_reported_ &&
        ALG_LOG_IS_ENABLED(alg::log::Level::Info)) {
        ALG_LOGI("svp_acl: split-plane %s %dx%d -> libyuv copy -> shared AIPP staging",
                 image.format == ALG_PIX_NV21 ? "NV21" : "NV12", image.width, image.height);
        split_copy_path_reported_ = true;
    }
#ifdef ALG_SVP_IVE_PREPROCESS
    if (use_ive_resize && !ive_path_reported_ && ALG_LOG_IS_ENABLED(alg::log::Level::Info)) {
        ALG_LOGI("svp_acl: split-plane %s %dx%d -> IVE resize %dx%d -> contiguous MMZ -> AIPP",
                 image.format == ALG_PIX_NV21 ? "NV21" : "NV12", image.width, image.height, aipp_src_width,
                 aipp_src_height);
        ive_path_reported_ = true;
    }
#endif

    timeval setup_begin{}, setup_end{};
    if (profile)
        gettimeofday(&setup_begin, nullptr);
    svp_acl_error ret = svp_acl_mdl_set_aipp_src_image_size(dynamic_aipp_, aipp_src_width, aipp_src_height);
    if (ret != SVP_ACL_SUCCESS)
        return ALG_E_PREPROCESS;

    int8_t uv_swap = 0;
    const svp_acl_aipp_input_format format = image.format == ALG_PIX_NV21 ? SVP_ACL_YVU420SP_U8 : SVP_ACL_YUV420SP_U8;
    ret = svp_acl_mdl_set_aipp_input_format(dynamic_aipp_, format);
    if (ret != SVP_ACL_SUCCESS && image.format == ALG_PIX_NV21) {
        /* 部分 CV610 工具链只接受 OM 声明的 YUV420SP 枚举。保持同一套 OM，
         * 把 NV21 当作 YUV420SP 读取，再由 rbuv_swap 交换 V/U。 */
        ret = svp_acl_mdl_set_aipp_input_format(dynamic_aipp_, SVP_ACL_YUV420SP_U8);
        uv_swap = 1;
    }
    if (ret != SVP_ACL_SUCCESS)
        return ALG_E_PREPROCESS;

    /* OpenCV NV12/NV21 默认的 BT.601 limited-range YUV -> full-range RGB。 */
    ret = svp_acl_mdl_set_aipp_csc_params(dynamic_aipp_, 1, 1192, 0, 1634, 1192, -400, -833, 1192, 2066, 0, 0, 0, 0, 16,
                                          128, 128);
    if (ret != SVP_ACL_SUCCESS)
        return ALG_E_PREPROCESS;
    ret = svp_acl_mdl_set_aipp_rbuv_swap_switch(dynamic_aipp_, uv_swap);
    if (ret != SVP_ACL_SUCCESS)
        return ALG_E_PREPROCESS;
    ret = svp_acl_mdl_set_aipp_ax_swap_switch(dynamic_aipp_, 0);
    if (ret != SVP_ACL_SUCCESS)
        return ALG_E_PREPROCESS;

    ret = svp_acl_mdl_set_aipp_crop_params(dynamic_aipp_, 0, 0, 0, 0, 0, 0);
    if (ret != SVP_ACL_SUCCESS)
        return ALG_E_PREPROCESS;
    ret = svp_acl_mdl_set_aipp_padding_params(dynamic_aipp_, 0, 0, 0, 0, 0, 0);
    if (ret != SVP_ACL_SUCCESS)
        return ALG_E_PREPROCESS;

    const bool resize = aipp_src_width != output_width || aipp_src_height != output_height;
    ret = svp_acl_mdl_set_aipp_scf_params(dynamic_aipp_, resize ? 2 : 0, aipp_src_width, aipp_src_height, output_width,
                                          output_height, 0);
    if (ret != SVP_ACL_SUCCESS)
        return ALG_E_PREPROCESS;

    ret = svp_acl_mdl_set_aipp_dtc_pixel_mean(dynamic_aipp_, cfg.mean[0], cfg.mean[1], cfg.mean[2], 0.0f, 0);
    if (ret != SVP_ACL_SUCCESS)
        return ALG_E_PREPROCESS;
    ret = svp_acl_mdl_set_aipp_dtc_pixel_min(dynamic_aipp_, 0.0f, 0.0f, 0.0f, 0.0f, 0);
    if (ret != SVP_ACL_SUCCESS)
        return ALG_E_PREPROCESS;
    ret = svp_acl_mdl_set_aipp_pixel_var_reci(dynamic_aipp_, cfg.scale / cfg.std[0], cfg.scale / cfg.std[1],
                                              cfg.scale / cfg.std[2], 1.0f, 0);
    if (ret != SVP_ACL_SUCCESS)
        return ALG_E_PREPROCESS;

    ret = svp_acl_mdl_set_aipp_by_input_index(model_id_, input_dataset_, image_input_index_, dynamic_aipp_);
    if (ret != SVP_ACL_SUCCESS) {
        ALG_LOGE("svp_acl: attach dynamic AIPP failed, ret=%d", ret);
        return ALG_E_PREPROCESS;
    }
    if (profile) {
        gettimeofday(&setup_end, nullptr);
        state.aipp_setup_ms = ElapsedMs(setup_begin, setup_end);
    }

    const size_t dst_size = svp_acl_mdl_get_input_size_by_index(model_desc_, image_input_index_);
    const size_t dst_stride = svp_acl_mdl_get_input_default_stride(model_desc_, image_input_index_);
    if (dst_size == 0 || dst_size > image_input_capacity_ || dst_stride < static_cast<size_t>(aipp_src_width) ||
        dst_stride > static_cast<size_t>(std::numeric_limits<int>::max())) {
        ALG_LOGE("svp_acl: invalid AIPP input buffer size=%zu stride=%zu capacity=%zu", dst_size, dst_stride,
                 image_input_capacity_);
        return ALG_E_PREPROCESS;
    }

    svp_acl_data_buffer* image_buffer = svp_acl_mdl_get_dataset_buffer(input_dataset_, image_input_index_);

#ifdef ALG_SVP_IVE_PREPROCESS
    if (use_ive_resize) {
        if ((source.stride[0] & 15) || (source.stride[1] & 15) || image.width > aipp_src_width * 16 ||
            image.height > aipp_src_height * 16 || aipp_src_width > image.width * 16 ||
            aipp_src_height > image.height * 16) {
            ALG_LOGE("svp_acl: IVE resize requires 16-byte strides and scale <= 16x");
            return ALG_E_PREPROCESS;
        }

        const size_t ive_stride = AlignUp(static_cast<size_t>(aipp_src_width), 16);
        Status ive_status = EnsureIveBuffers(aipp_src_width, aipp_src_height, ive_stride);
        if (ive_status != ALG_OK)
            return ive_status;

        ot_svp_src_img ive_src{};
        ive_src.type = OT_SVP_IMG_TYPE_YUV420SP;
        ive_src.width = static_cast<td_u32>(image.width);
        ive_src.height = static_cast<td_u32>(image.height);
        ive_src.stride[0] = static_cast<td_u32>(source.stride[0]);
        ive_src.stride[1] = static_cast<td_u32>(source.stride[1]);
        ive_src.phys_addr[0] = source.phys_addr[0];
        ive_src.phys_addr[1] = source.phys_addr[1];
        ive_src.virt_addr[0] = static_cast<td_u64>(reinterpret_cast<uintptr_t>(source.plane[0]));
        ive_src.virt_addr[1] = static_cast<td_u64>(reinterpret_cast<uintptr_t>(source.plane[1]));

        const size_t ive_y_bytes = ive_stride * static_cast<size_t>(aipp_src_height);
        const size_t ive_bytes = ive_y_bytes + ive_stride * static_cast<size_t>(aipp_src_height / 2);
        if (ive_bytes > ive_output_capacity_ || ive_bytes > dst_size) {
            ALG_LOGE("svp_acl: IVE output size=%zu exceeds buffer/model input=%zu/%zu", ive_bytes, ive_output_capacity_,
                     dst_size);
            return ALG_E_PREPROCESS;
        }

        ot_svp_dst_img ive_dst{};
        ive_dst.type = OT_SVP_IMG_TYPE_YUV420SP;
        ive_dst.width = static_cast<td_u32>(aipp_src_width);
        ive_dst.height = static_cast<td_u32>(aipp_src_height);
        ive_dst.stride[0] = static_cast<td_u32>(ive_stride);
        ive_dst.stride[1] = static_cast<td_u32>(ive_stride);
        ive_dst.phys_addr[0] = static_cast<td_u64>(ive_output_phys_);
        ive_dst.phys_addr[1] = static_cast<td_u64>(ive_output_phys_) + ive_y_bytes;
        ive_dst.virt_addr[0] = static_cast<td_u64>(reinterpret_cast<uintptr_t>(ive_output_virt_));
        ive_dst.virt_addr[1] = ive_dst.virt_addr[0] + ive_y_bytes;

        ot_ive_resize_ctrl ive_ctrl{};
        ive_ctrl.mode = OT_IVE_RESIZE_MODE_LINEAR;
        ive_ctrl.num = 1;
        ive_ctrl.mem.phys_addr = static_cast<td_u64>(ive_assist_phys_);
        ive_ctrl.mem.virt_addr = static_cast<td_u64>(reinterpret_cast<uintptr_t>(ive_assist_virt_));
        ive_ctrl.mem.size = static_cast<td_u32>(ive_assist_capacity_);

        timeval ive_begin{}, ive_end{};
        if (profile)
            gettimeofday(&ive_begin, nullptr);
        ot_ive_handle ive_handle = 0;
        td_s32 ive_ret = ss_mpi_ive_resize(&ive_handle, &ive_src, &ive_dst, &ive_ctrl, TD_TRUE);
        if (ive_ret != TD_SUCCESS) {
            ALG_LOGE("svp_acl: ss_mpi_ive_resize failed, ret=%d", ive_ret);
            return ALG_E_PREPROCESS;
        }
        td_bool ive_finished = TD_FALSE;
        do {
            ive_ret = ss_mpi_ive_query(ive_handle, &ive_finished, TD_TRUE);
            if (ive_ret == OT_ERR_IVE_QUERY_TIMEOUT)
                usleep(100);
        } while (ive_ret == OT_ERR_IVE_QUERY_TIMEOUT);
        if (ive_ret != TD_SUCCESS || ive_finished != TD_TRUE) {
            ALG_LOGE("svp_acl: wait IVE resize failed, ret=%d finished=%d", ive_ret, static_cast<int>(ive_finished));
            return ALG_E_PREPROCESS;
        }
        if (profile) {
            gettimeofday(&ive_end, nullptr);
            state.aipp_ive_ms = ElapsedMs(ive_begin, ive_end);
        }

        timeval bind_begin{}, bind_end{};
        if (profile)
            gettimeofday(&bind_begin, nullptr);
        ret = svp_acl_update_data_buffer(image_buffer, ive_output_virt_, ive_bytes, ive_stride);
        if (ret != SVP_ACL_SUCCESS) {
            ALG_LOGE("svp_acl: bind IVE output to AIPP failed, ret=%d", ret);
            return ALG_E_PREPROCESS;
        }
        input_views_[0].data = ive_output_virt_;
        input_views_[0].size_bytes = ive_bytes;
        if (own_image_input_ && own_image_input_ != ive_output_virt_) {
            (void)svp_acl_rt_free(own_image_input_);
            input_allocations_[image_input_index_] = nullptr;
            own_image_input_ = nullptr;
        }
        if (profile) {
            gettimeofday(&bind_end, nullptr);
            state.aipp_bind_ms = ElapsedMs(bind_begin, bind_end);
        }
        image_input_preflushed_ = true;

        state.aipp_profile_valid = profile;
        state.aipp_staging_reused = false;
        state.aipp_split_planes = true;
        state.aipp_external_zero_copy = false;
        state.aipp_ive_resize = true;
        state.aipp_staging_bytes = ive_bytes;
        state.aipp_staging_stride = ive_stride;
        state.scale_ratio = scale_ratio;
        state.pad_left = pad_left;
        state.pad_top = pad_top;
        state.original_width = image.width;
        state.original_height = image.height;
        return ALG_OK;
    }
#endif

#ifdef ALG_SVP_LIBYUV_FALLBACK
    /* 只有 libyuv 合并路径往这里写；can_share 分支直接绑定 shared_staging->data。 */
    uint8_t* dst = static_cast<uint8_t*>(svp_acl_get_data_buffer_addr(image_buffer));
#endif
    size_t dst_y_bytes = 0;
    size_t dst_uv_bytes = 0;
    if (!CheckedPlaneByteSize(static_cast<int>(dst_stride), aipp_src_height, &dst_y_bytes) ||
        !CheckedPlaneByteSize(static_cast<int>(dst_stride), aipp_src_height / 2, &dst_uv_bytes) ||
        dst_y_bytes > std::numeric_limits<size_t>::max() - dst_uv_bytes) {
        return ALG_E_PREPROCESS;
    }
    const size_t needed_dst = dst_y_bytes + dst_uv_bytes;
    if (needed_dst > dst_size)
        return ALG_E_PREPROCESS;

    /* 连续 NV12/NV21 由调用方通过 SYS mmap 提供非 cached MMZ/VB 映射。
     * data buffer 只借用该地址；像素由 VPSS 写入，SDK 不复制、不 flush、也不释放。
     * 需要 CPU 预缩放时不能走零拷贝：必须读源、缩放后写入 staging，见下方。 */
    if (!copy_split_planes && !cpu_prescale) {
        size_t external_y_bytes = 0;
        size_t external_uv_bytes = 0;
        if (source.stride[0] != source.stride[1] ||
            !CheckedPlaneByteSize(source.stride[0], image.height, &external_y_bytes) ||
            !CheckedPlaneByteSize(source.stride[1], image.height / 2, &external_uv_bytes) ||
            external_y_bytes > std::numeric_limits<size_t>::max() - external_uv_bytes) {
            ALG_LOGE("svp_acl: external YUV mapping has incompatible plane strides");
            return ALG_E_PREPROCESS;
        }
        const size_t external_bytes = external_y_bytes + external_uv_bytes;
        if (external_bytes > dst_size) {
            ALG_LOGE("svp_acl: external YUV mapping size=%zu exceeds model input=%zu", external_bytes, dst_size);
            return ALG_E_PREPROCESS;
        }

        state.aipp_profile_valid = profile;
        state.aipp_staging_reused = false;
        state.aipp_split_planes = false;
        state.aipp_external_zero_copy = true;
        state.aipp_staging_bytes = external_bytes;
        state.aipp_staging_stride = static_cast<size_t>(source.stride[0]);

        timeval bind_begin{}, bind_end{};
        if (profile)
            gettimeofday(&bind_begin, nullptr);
        void* external_data = const_cast<uint8_t*>(source.plane[0]);
        ret = svp_acl_update_data_buffer(image_buffer, external_data, external_bytes,
                                         static_cast<size_t>(source.stride[0]));
        if (ret != SVP_ACL_SUCCESS) {
            ALG_LOGE("svp_acl: bind external YUV mapping failed, ret=%d", ret);
            return ALG_E_PREPROCESS;
        }
        input_views_[0].data = external_data;
        input_views_[0].size_bytes = external_bytes;

        /* dataset buffer 已切换到外部地址，释放不再使用的私有图像输入；释放表中
         * 不记录 external_data，避免 FreeAll 误释放调用方的 SYS mmap。 */
        if (own_image_input_ && own_image_input_ != external_data) {
            (void)svp_acl_rt_free(own_image_input_);
            input_allocations_[image_input_index_] = nullptr;
            own_image_input_ = nullptr;
        }
        if (profile) {
            gettimeofday(&bind_end, nullptr);
            state.aipp_bind_ms = ElapsedMs(bind_begin, bind_end);
        }
        image_input_preflushed_ = true;

        state.scale_ratio = scale_ratio;
        state.pad_left = pad_left;
        state.pad_top = pad_top;
        state.original_width = image.width;
        state.original_height = image.height;
        return ALG_OK;
    }

    /* 预缩放时 staging 里装的是缩放后的帧，尺寸与源不同，跨模型共享的前提不再成立；
     * 各模型用自己的私有 staging，避免把别的模型的尺寸当作可复用条件。 */
    const bool can_share = !cpu_prescale && shared_staging && shared_staging->flushed &&
        SameSource(*shared_staging, image, source) && shared_staging->staged_width == aipp_src_width &&
        shared_staging->staged_height == aipp_src_height && shared_staging->data &&
        shared_staging->capacity >= dst_size && shared_staging->data_size == needed_dst &&
        shared_staging->row_stride == dst_stride;

    state.aipp_profile_valid = profile;
    state.aipp_staging_reused = can_share;
    state.aipp_split_planes = copy_split_planes;
    state.aipp_external_zero_copy = false;
    state.aipp_staging_bytes = needed_dst;
    state.aipp_staging_stride = dst_stride;

    timeval bind_begin{}, bind_end{};
    if (profile)
        gettimeofday(&bind_begin, nullptr);

    if (can_share) {
        ret = svp_acl_update_data_buffer(image_buffer, shared_staging->data, dst_size, dst_stride);
        if (ret != SVP_ACL_SUCCESS)
            return ALG_E_PREPROCESS;
        input_views_[0].data = shared_staging->data;
        input_views_[0].size_bytes = dst_size;
        image_input_preflushed_ = true;

        /* 本 inferer 后续始终由同一静态 chain 顺序取得共享帧，可释放不再使用的
         * 私有整帧缓存。dataset buffer 不拥有地址，FreeAll 只释放 allocations_。 */
        bool released_private = false;
        if (own_image_input_ && own_image_input_ != shared_staging->data) {
            (void)svp_acl_rt_free(own_image_input_);
            input_allocations_[image_input_index_] = nullptr;
            own_image_input_ = nullptr;
            released_private = true;
        }
        if (profile) {
            gettimeofday(&bind_end, nullptr);
            state.aipp_bind_ms = ElapsedMs(bind_begin, bind_end);
        }
        if (released_private) {
            ALG_LOGI("svp_acl: switched to shared AIPP staging, released private input");
        }
    } else {
#ifdef ALG_SVP_LIBYUV_FALLBACK
        Status own_status = EnsureOwnImageInput();
        if (own_status != ALG_OK)
            return own_status;
        dst = static_cast<uint8_t*>(own_image_input_);
        ret = svp_acl_update_data_buffer(image_buffer, dst, dst_size, dst_stride);
        if (ret != SVP_ACL_SUCCESS)
            return ALG_E_PREPROCESS;
        input_views_[0].data = dst;
        input_views_[0].size_bytes = dst_size;
        if (profile) {
            gettimeofday(&bind_end, nullptr);
            state.aipp_bind_ms = ElapsedMs(bind_begin, bind_end);
        }

        timeval copy_begin{}, copy_end{};
        double copy_cpu_begin = 0.0;
        if (profile) {
            gettimeofday(&copy_begin, nullptr);
            copy_cpu_begin = ThreadCpuMs();
        }
        uint8_t* dst_uv = dst + dst_y_bytes;
        int libyuv_status = 0;
        /* 本次调用产出的 1/2 折半中间帧；非空时发布到 SharedInputStaging，
         * 供同一帧内中间帧尺寸相同的后续模型复用。 */
        void* prescale_base_data = nullptr;
        int prescale_base_w = 0;
        int prescale_base_h = 0;
        int prescale_base_stride = 0;
        if (cpu_prescale) {
            /* NV12Scale 内部对 UV 平面调用 UVScale(..., width/2, ...)，按「UV 对」
             * 缩放、不解释 U/V 字节语义，所以 NV12 与 NV21 通用。一次调用同时完成
             * 「分平面合并」与「缩放到 aipp_output_size」；ResolveImageView 已保证
             * 连续帧与分平面两种输入下 plane[0]/plane[1]/stride 都成立。
             *
             * 1080p -> 640x360/568x320 约为 1/3，libyuv 没有 1/3 专用路径，会落到
             * 通用 ScalePlaneBox；该路径的水平累加在 ARM 上没有 NEON，单次约 10ms。
             * 这里先用精确 1/2（ScalePlaneDown2，纯 NEON）折半到仍大于目标的中间
             * 尺寸，再由中间帧缩到目标，避开标量大比例滤波。1080p 折一次到
             * 960x540，4K 折两次；中间帧用乒乓缓冲复用。 */
            int hops = PrescaleHalfSteps(image.width, image.height, aipp_src_width, aipp_src_height);
            int base_w = image.width;
            int base_h = image.height;
            for (int i = 0; i < hops; ++i) {
                base_w /= 2;
                base_h /= 2;
            }

            /* 同一帧内两个全图模型常常需要同一张中间帧（如 1080p -> 960x540）。
             * 先跑的模型算完发布，后跑的模型直接复用，少做一次大图折半。 */
            const bool reuse_base = hops > 0 && shared_staging && shared_staging->prescale_data &&
                shared_staging->prescale_width == base_w && shared_staging->prescale_height == base_h &&
                shared_staging->prescale_stride > 0 && SameSource(*shared_staging, image, source);

            if (!reuse_base && hops > 0) {
                int w = image.width;
                int h = image.height;
                for (int i = 0; i < hops; ++i) {
                    w /= 2;
                    h /= 2;
                    const size_t stride = (static_cast<size_t>(w) + 63) & ~static_cast<size_t>(63);
                    const size_t bytes = stride * static_cast<size_t>(h) + stride * static_cast<size_t>(h / 2);
                    if (EnsurePrescaleTmp(i & 1, bytes) != ALG_OK) {
                        /* 分配失败时退回单次直缩，保持功能可用。 */
                        hops = 0;
                        break;
                    }
                }
            }

            if (!cpu_prescale_reported_ && ALG_LOG_IS_ENABLED(alg::log::Level::Info)) {
                ALG_LOGI("svp_acl: CPU pre-scale %s %dx%d stride=%d/%d sep=%d data_len=%d data=%p -> %dx%d "
                         "(libyuv kFilterBox, half_steps=%d, reused_base=%d) -> AIPP scf=0",
                         image.format == ALG_PIX_NV21 ? "NV21" : "NV12", image.width, image.height, source.stride[0],
                         source.stride[1], source.uses_separate_planes ? 1 : 0, image.data_len, image.data,
                         aipp_src_width, aipp_src_height, reuse_base ? 0 : hops, reuse_base ? 1 : 0);
                cpu_prescale_reported_ = true;
            }

            if (reuse_base) {
                const uint8_t* base = static_cast<const uint8_t*>(shared_staging->prescale_data);
                const int stride = shared_staging->prescale_stride;
                libyuv_status = libyuv::NV12Scale(
                    base, stride, base + static_cast<size_t>(stride) * static_cast<size_t>(base_h), stride, base_w,
                    base_h, dst, static_cast<int>(dst_stride), dst_uv, static_cast<int>(dst_stride), aipp_src_width,
                    aipp_src_height, libyuv::kFilterBox);
            } else if (hops > 0) {
                const uint8_t* cur_y = source.plane[0];
                const uint8_t* cur_uv = source.plane[1];
                int cur_stride_y = source.stride[0];
                int cur_stride_uv = source.stride[1];
                int cur_w = image.width;
                int cur_h = image.height;
                for (int i = 0; i < hops && libyuv_status == 0; ++i) {
                    const int next_w = cur_w / 2;
                    const int next_h = cur_h / 2;
                    const int stride =
                        static_cast<int>((static_cast<size_t>(next_w) + 63) & ~static_cast<size_t>(63));
                    uint8_t* buf = prescale_tmp_[i & 1];
                    uint8_t* buf_uv = buf + static_cast<size_t>(stride) * static_cast<size_t>(next_h);
                    libyuv_status = libyuv::NV12Scale(cur_y, cur_stride_y, cur_uv, cur_stride_uv, cur_w, cur_h, buf,
                                                     stride, buf_uv, stride, next_w, next_h, libyuv::kFilterBox);
                    cur_y = buf;
                    cur_uv = buf_uv;
                    cur_stride_y = stride;
                    cur_stride_uv = stride;
                    cur_w = next_w;
                    cur_h = next_h;
                }
                if (libyuv_status == 0) {
                    libyuv_status = libyuv::NV12Scale(
                        cur_y, cur_stride_y, cur_uv, cur_stride_uv, cur_w, cur_h, dst,
                        static_cast<int>(dst_stride), dst_uv, static_cast<int>(dst_stride), aipp_src_width,
                        aipp_src_height, libyuv::kFilterBox);
                }
                if (libyuv_status == 0) {
                    prescale_base_data = prescale_tmp_[(hops - 1) & 1];
                    prescale_base_w = base_w;
                    prescale_base_h = base_h;
                    prescale_base_stride =
                        static_cast<int>((static_cast<size_t>(base_w) + 63) & ~static_cast<size_t>(63));
                }
            } else {
                libyuv_status =
                    libyuv::NV12Scale(source.plane[0], source.stride[0], source.plane[1], source.stride[1],
                                      image.width, image.height, dst, static_cast<int>(dst_stride), dst_uv,
                                      static_cast<int>(dst_stride), aipp_src_width, aipp_src_height,
                                      libyuv::kFilterBox);
            }
        } else {
            libyuv_status = image.format == ALG_PIX_NV21
                ? libyuv::NV21Copy(source.plane[0], source.stride[0], source.plane[1], source.stride[1], dst,
                                   static_cast<int>(dst_stride), dst_uv, static_cast<int>(dst_stride), image.width,
                                   image.height)
                : libyuv::NV12Copy(source.plane[0], source.stride[0], source.plane[1], source.stride[1], dst,
                                   static_cast<int>(dst_stride), dst_uv, static_cast<int>(dst_stride), image.width,
                                   image.height);
        }
        if (libyuv_status != 0) {
            ALG_LOGE("svp_acl: libyuv %s failed, ret=%d", cpu_prescale ? "pre-scale" : "plane copy", libyuv_status);
            return ALG_E_PREPROCESS;
        }
        if (profile) {
            gettimeofday(&copy_end, nullptr);
            state.aipp_copy_ms = ElapsedMs(copy_begin, copy_end);
            state.aipp_copy_cpu_ms = ThreadCpuMs() - copy_cpu_begin;
        }

        /* 只同步当前原图对应的 YUV 区域，不 flush 按最大输入预留的剩余空间。 */
        timeval flush_begin{}, flush_end{};
        if (profile)
            gettimeofday(&flush_begin, nullptr);
        ret = svp_acl_rt_mem_flush(dst, needed_dst);
        if (ret != SVP_ACL_SUCCESS) {
            ALG_LOGE("svp_acl: staged AIPP input flush failed, ret=%d", ret);
            return ALG_E_BACKEND;
        }
        if (profile) {
            gettimeofday(&flush_end, nullptr);
            state.aipp_flush_ms = ElapsedMs(flush_begin, flush_end);
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
            if (prescale_base_data) {
                shared_staging->prescale_data = prescale_base_data;
                shared_staging->prescale_width = prescale_base_w;
                shared_staging->prescale_height = prescale_base_h;
                shared_staging->prescale_stride = prescale_base_stride;
            }
            shared_staging->staged_width = aipp_src_width;
            shared_staging->staged_height = aipp_src_height;
            shared_staging->data = dst;
            shared_staging->capacity = image_input_capacity_;
            shared_staging->data_size = needed_dst;
            shared_staging->row_stride = dst_stride;
            shared_staging->flushed = true;
        }
#else
        /* 本目标未编入 libyuv（ALG_SVP_LIBYUV_FALLBACK=OFF，或该架构没有可用的
         * 预编译库）：Y/UV 分平面输入无法合并成 AIPP 要求的连续 NV12/NV21。
         * 请由调用方传入连续帧（走上面的共享 staging 零拷贝路径），或改用
         * -DALG_SVP_LIBYUV_FALLBACK=ON 并提供对应架构的 libyuv。 */
        ALG_LOGE("svp_acl: split-plane AIPP staging is not compiled in "
                 "(ALG_SVP_LIBYUV_FALLBACK=OFF); pass a contiguous NV12/NV21 frame");
        return ALG_E_PREPROCESS;
#endif
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
