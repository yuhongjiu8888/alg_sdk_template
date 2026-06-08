#include "backend/xmm/xmm_inferer.h"

#include <cstdio>
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
        case XMEDIA_CL_UINT16:  return DataType::kU16;
        case XMEDIA_CL_FP16:    return DataType::kF16;
        case XMEDIA_CL_INT32:   return DataType::kI32;
        default:
            /* 静默落到 kU8 会把 FP16 等当成单字节，按 (raw-zp)*scale 解出全是
             * 垃圾值 → 后处理 obj 全低 → 一个框都检不出。宁可吵也别静默。 */
            ALG_LOGW("MapDtype: unknown xmm dtype=%d, fallback kU8 (likely wrong)", xmm_dtype);
            return DataType::kU8;
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
    /* 必须用 calloc：current_batch / tensor_batch 会被 set_inout 当 batch 配置读取，
     * malloc 留下的脏值会让 NPU 用错误的 batch 跑图，infer 不报错但输出是垃圾。
     * 对齐板端 demo（sample_speedsignnet.cpp::alloc_inout_mem）与 test_xmm.c。 */
    io->tensor = static_cast<xmedia_cl_tensor*>(calloc(io->num, sizeof(xmedia_cl_tensor)));
    io->current_batch = static_cast<xmedia_cl_u32*>(calloc(io->num, sizeof(xmedia_cl_u32)));
    io->tensor_batch = static_cast<xmedia_cl_tensor_batch*>(calloc(io->num, sizeof(xmedia_cl_tensor_batch)));
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
                /* 对齐 demo：输出 buffer 先清零（process 后直接读，不再 flush 输出）。 */
                memset(base, 0, cl_output_.tensor[i].size);
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

    /* —— 一次性诊断：板端检不出框时先确认 NPU tensor 元数据 ——
     * 重点看 output：
     *   type=4 → FP16（之前 MapDtype 漏了会当 U8 解，全是垃圾值）；
     *   dims 与 pch 不一致 → NPU 对维度做了对齐补位（pitch != dims），
     *     后处理按密排 idx 取数会错位 → 也读出垃圾。
     * 确认后可删掉这段。 */
    auto dump = [](const char* tag, const xmedia_cl_tensor_info_inout& io) {
        for (xmedia_cl_u32 i = 0; i < io.num; ++i) {
            const xmedia_cl_tensor& t = io.tensor[i];
            char dims[64] = {0}, pch[64] = {0};
            int dn = 0, pn = 0;
            for (xmedia_cl_u32 d = 0; d < t.shape.ndims && dn < 56; ++d)
                dn += snprintf(dims + dn, sizeof(dims) - dn, "%u,", t.shape.dims[d]);
            for (xmedia_cl_u32 d = 0; d < t.shape.ndims && pn < 56; ++d)
                pn += snprintf(pch + pn, sizeof(pch) - pn, "%u,", t.shape.pch[d]);
            ALG_LOGW("[xmm-diag] %s[%u] name=%s type=%d ndims=%u dims=[%s] pch=[%s] "
                     "scale=%g zp=%d size=%u",
                     tag, i, t.name ? (const char*)t.name : "", (int)t.shape.type,
                     t.shape.ndims, dims, pch, t.quant.scale, (int)t.quant.zp, t.size);
        }
    };
    dump("in", cl_input_);
    dump("out", cl_output_);
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

    /* 不对输出再做 flush_cache：板端 demo（session_run）只 flush 输入。
     * output buffer 已在 Load 时 memset 清零，process 后直接读即可；
     * 若此处对输出做 clean 型 flush，会把脏 cache 写回覆盖 NPU 结果。 */

    for (xmedia_cl_u32 i = 0; i < cl_output_.num; ++i) {
        output_views_[i].data = cl_output_.tensor[i].addr;
    }
    return ALG_OK;
}

}  // namespace alg
