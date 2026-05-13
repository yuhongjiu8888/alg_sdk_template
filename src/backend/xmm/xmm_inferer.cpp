#include "backend/xmm/xmm_inferer.h"

#include <cstdlib>
#include <cstring>

#include "core/logger.h"

#define ALIGN_BYTES 8
#define ALIGN_UP(A, B) ((((A) % (B)) == 0) ? (A) : ((A) + (B) - ((A) % (B))))

#define XMM_TRY(call, msg)                                              \
    do {                                                                \
        xmedia_cl_s32 _r = (call);                                      \
        if (_r != XMEDIA_CL_SUCCESS) {                                  \
            ALG_LOGE("%s failed, ret=%d", msg, _r);                     \
            return ALG_E_BACKEND;                                       \
        }                                                               \
    } while (0)

namespace alg {

namespace {

DataType MapDtype(int xmm_dtype) {
    switch (xmm_dtype) {
        case XMEDIA_CL_UINT8:   return DataType::kU8;
        case XMEDIA_CL_INT8:    return DataType::kI8;
        case XMEDIA_CL_INT16:   return DataType::kI16;
        case XMEDIA_CL_FLOAT16: return DataType::kF16;
        case XMEDIA_CL_FLOAT32: return DataType::kF32;
        default:                return DataType::kU8;
    }
}

int XmmMmzAllocCached(xmedia_u64* phy, void** vir, const char* name, unsigned size) {
    *phy = xmedia_mmz_alloc(NULL, name, size);
    if (*phy == 0) { ALG_LOGE("mmz_alloc(%s, %u) failed", name, size); return -1; }
    *vir = xmedia_mmz_map(*phy, size, 1);
    if (*vir == NULL) {
        xmedia_mmz_free(*phy);
        *phy = 0;
        ALG_LOGE("mmz_map(%s) failed", name);
        return -1;
    }
    return 0;
}

void XmmMmzFree(xmedia_u64& phy, void*& vir) {
    if (vir) xmedia_mmz_unmap(vir);
    if (phy) xmedia_mmz_free(phy);
    vir = nullptr;
    phy = 0;
}

int AllocInoutInfo(xmedia_cl_tensor_info_inout* io) {
    io->tensor = static_cast<xmedia_cl_tensor*>(malloc(sizeof(xmedia_cl_tensor) * io->num));
    io->current_batch = static_cast<xmedia_cl_u32*>(malloc(sizeof(xmedia_cl_u32) * io->num));
    io->tensor_batch = static_cast<xmedia_cl_tensor_batch*>(malloc(sizeof(xmedia_cl_tensor_batch) * io->num));
    if (!io->tensor || !io->current_batch || !io->tensor_batch) return XMEDIA_CL_OUT_OF_HOST_MEMORY;
    return XMEDIA_CL_SUCCESS;
}

void FreeInoutInfo(xmedia_cl_tensor_info_inout* io) {
    free(io->tensor);          io->tensor = nullptr;
    free(io->current_batch);   io->current_batch = nullptr;
    free(io->tensor_batch);    io->tensor_batch = nullptr;
}

}  // namespace

XmmInferer::XmmInferer() {
    memset(&cl_input_, 0, sizeof(cl_input_));
    memset(&cl_output_, 0, sizeof(cl_output_));
}

XmmInferer::~XmmInferer() { FreeAll(); }

void XmmInferer::FreeAll() {
    if (!initialized_ && !context_ && !devices_ && !vir_input_ && !vir_output_ &&
        !vir_workspace_ && !vir_weight_) {
        return;
    }
    FreeInoutInfo(&cl_input_);
    FreeInoutInfo(&cl_output_);
    if (graph_) { xmedia_cl_graph_unload(graph_); graph_ = nullptr; }
    XmmMmzFree(phy_workspace_, vir_workspace_);
    XmmMmzFree(phy_weight_, vir_weight_);
    XmmMmzFree(phy_input_, vir_input_);
    XmmMmzFree(phy_output_, vir_output_);
    if (context_) { xmedia_cl_release_context(context_); context_ = nullptr; }
    if (devices_) {
        xmedia_cl_release_device_ids(devices_, &num_devices_);
        free(devices_);
        devices_ = nullptr;
    }
    xmedia_cl_uninit();
    xmedia_sys_exit();
    initialized_ = false;
}

Status XmmInferer::Load(const std::string& model_path) {
    if (initialized_) return ALG_E_INVALID_ARG;

    XMM_TRY(xmedia_sys_init(XMEDIA_NULL), "xmedia_sys_init");

    xmedia_cl_s32 ret = xmedia_cl_init();
    if (ret != XMEDIA_CL_SUCCESS) {
        xmedia_sys_exit();
        ALG_LOGE("xmedia_cl_init=%d", ret);
        return ALG_E_BACKEND;
    }

    Status status = ALG_E_BACKEND;
    do {
        ret = xmedia_cl_get_device_ids(XMEDIA_CL_DEVICE_ALL, NULL, &num_devices_);
        if (ret != XMEDIA_CL_SUCCESS) { ALG_LOGE("get_device_ids(count)=%d", ret); break; }
        devices_ = static_cast<xmedia_cl_device_id*>(calloc(num_devices_, sizeof(xmedia_cl_device_id)));
        if (!devices_) { ALG_LOGE("calloc devices failed"); break; }
        ret = xmedia_cl_get_device_ids(XMEDIA_CL_DEVICE_ALL, devices_, &num_devices_);
        if (ret != XMEDIA_CL_SUCCESS) { ALG_LOGE("get_device_ids=%d", ret); break; }

        xmedia_cl_s32 err = 0;
        context_ = xmedia_cl_create_context(num_devices_, devices_, &err);
        if (err != XMEDIA_CL_SUCCESS) { ALG_LOGE("create_context=%d", err); break; }

        xmedia_cl_u32 worksize = 0, weightsize = 0;
        ret = xmedia_cl_graph_querysize_from_file(model_path.c_str(), &worksize, &weightsize);
        if (ret != XMEDIA_CL_SUCCESS) { ALG_LOGE("querysize=%d", ret); break; }

        if (worksize && XmmMmzAllocCached(&phy_workspace_, &vir_workspace_, "npu_ws", worksize) != 0) break;
        if (weightsize && XmmMmzAllocCached(&phy_weight_, &vir_weight_, "npu_wt", weightsize) != 0) break;

        ret = xmedia_cl_graph_loadmodel_from_file_withmem(&context_, model_path.c_str(),
                                                          vir_workspace_, worksize,
                                                          vir_weight_, weightsize, &graph_);
        if (ret != XMEDIA_CL_SUCCESS) { ALG_LOGE("loadmodel=%d", ret); break; }

        /* Inputs: two-pass query. */
        ret = xmedia_cl_graph_get_input(graph_, 0, &cl_input_);
        if (ret != XMEDIA_CL_SUCCESS) { ALG_LOGE("get_input(count)=%d", ret); break; }
        if (AllocInoutInfo(&cl_input_) != XMEDIA_CL_SUCCESS) break;
        ret = xmedia_cl_graph_get_input(graph_, cl_input_.num, &cl_input_);
        if (ret != XMEDIA_CL_SUCCESS) { ALG_LOGE("get_input=%d", ret); break; }

        /* Outputs: two-pass query. */
        ret = xmedia_cl_graph_get_output(graph_, 0, &cl_output_);
        if (ret != XMEDIA_CL_SUCCESS) { ALG_LOGE("get_output(count)=%d", ret); break; }
        if (AllocInoutInfo(&cl_output_) != XMEDIA_CL_SUCCESS) break;
        ret = xmedia_cl_graph_get_output(graph_, cl_output_.num, &cl_output_);
        if (ret != XMEDIA_CL_SUCCESS) { ALG_LOGE("get_output=%d", ret); break; }

        /* MMZ allocations for I/O. */
        input_total_size_ = 0;
        for (xmedia_cl_u32 i = 0; i < cl_input_.num; ++i)
            input_total_size_ += ALIGN_UP(cl_input_.tensor[i].size, ALIGN_BYTES);
        if (XmmMmzAllocCached(&phy_input_, &vir_input_, "npu_in", input_total_size_) != 0) break;

        output_total_size_ = 0;
        for (xmedia_cl_u32 i = 0; i < cl_output_.num; ++i)
            output_total_size_ += ALIGN_UP(cl_output_.tensor[i].size, ALIGN_BYTES);
        if (XmmMmzAllocCached(&phy_output_, &vir_output_, "npu_out", output_total_size_) != 0) break;

        {
            char* base = static_cast<char*>(vir_input_);
            for (xmedia_cl_u32 i = 0; i < cl_input_.num; ++i) {
                cl_input_.tensor[i].addr = base;
                base += ALIGN_UP(cl_input_.tensor[i].size, ALIGN_BYTES);
            }
            base = static_cast<char*>(vir_output_);
            for (xmedia_cl_u32 i = 0; i < cl_output_.num; ++i) {
                cl_output_.tensor[i].addr = base;
                base += ALIGN_UP(cl_output_.tensor[i].size, ALIGN_BYTES);
            }
        }

        ret = xmedia_cl_graph_set_inout(graph_, &cl_input_, &cl_output_);
        if (ret != XMEDIA_CL_SUCCESS) { ALG_LOGE("set_inout=%d", ret); break; }

        initialized_ = true;
        status = BuildViews();
    } while (false);

    if (status != ALG_OK) FreeAll();
    return status;
}

Status XmmInferer::BuildViews() {
    auto fill = [](const xmedia_cl_tensor& t) {
        TensorView v;
        v.name = t.name ? t.name : "";
        v.dtype = MapDtype(t.shape.type);
        v.layout = Layout::kNCHW;
        v.shape.ndims = static_cast<int>(t.shape.ndims);
        for (xmedia_cl_u32 i = 0; i < t.shape.ndims && (int)i < Shape::kMaxDims; ++i)
            v.shape.dims[i] = static_cast<int>(t.shape.dims[i]);
        v.quant.scale = t.quant.scale;
        v.quant.zero_point = t.quant.zp;
        v.quant.quantized = (v.dtype == DataType::kU8 || v.dtype == DataType::kI8);
        v.data = t.addr;
        v.size_bytes = t.size;
        return v;
    };
    input_views_.clear();
    output_views_.clear();
    for (xmedia_cl_u32 i = 0; i < cl_input_.num; ++i)  input_views_.push_back(fill(cl_input_.tensor[i]));
    for (xmedia_cl_u32 i = 0; i < cl_output_.num; ++i) output_views_.push_back(fill(cl_output_.tensor[i]));
    return ALG_OK;
}

Status XmmInferer::Forward() {
    if (!initialized_) return ALG_E_NOT_INITIALIZED;

    xmedia_mmz_flush_cache(phy_input_, vir_input_, input_total_size_);

    xmedia_cl_s32 ret = xmedia_cl_graph_process(graph_);
    if (ret != XMEDIA_CL_SUCCESS) {
        ALG_LOGE("graph_process=%d", ret);
        return ALG_E_BACKEND;
    }

    xmedia_mmz_flush_cache(phy_output_, vir_output_, output_total_size_);

    for (xmedia_cl_u32 i = 0; i < cl_output_.num; ++i) {
        output_views_[i].data = cl_output_.tensor[i].addr;
    }
    return ALG_OK;
}

}  // namespace alg
