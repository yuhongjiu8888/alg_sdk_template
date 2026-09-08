/**
 * @file alg_interface.h
 * @brief 算法 SDK 的公共 C 入口。
 *
 * 整个 SDK 从一份 JSON 配置启动：JSON 里描述了 solution 编排（哪几个模型、
 * 怎么串）、每个模型走哪个后端的什么模型文件、前/后处理参数。
 *
 * 使用：
 *   AlgHandle h = NULL;
 *   AlgCreate(&h, "/data/speed_limit.json");
 *   AlgRun(h, &image, &result);
 *   AlgFreeResult(&result);
 *   AlgDestroy(h);
 */

#ifndef ALG_INTERFACE_H
#define ALG_INTERFACE_H

#include "alg_types.h"

ALG_C_BEGIN

/** 从 JSON 配置创建 SDK 实例。 */
ALG_API AlgStatus AlgCreate(AlgHandle* handle, const char* config_json_path);

/** 释放 SDK 实例及其所有资源。 */
ALG_API AlgStatus AlgDestroy(AlgHandle handle);

/** 对一帧图像跑完整条 solution 流水线。 */
ALG_API AlgStatus AlgRun(AlgHandle handle, const AlgImage* image, AlgResult* result);

/**
 * 使用上层 VPSS 原始分辨率帧执行完整流水线。当前配置只需 source_id=0；
 * 接口保留帧集合形式以兼容扩展。支持 NV12/NV21，默认部署格式为 NV21。
 */
ALG_API AlgStatus AlgRunNative(AlgHandle handle, const AlgNativeFrameSet* frames,
                               AlgResult* result);

/**
 * 查询 AlgRunNative 所需的 VPSS 输入。*count 传入数组容量并返回实际数量；
 * requirements 为 NULL 时只查询数量。当前配置返回一路原始尺寸输入。
 */
ALG_API AlgStatus AlgGetInputRequirements(AlgHandle handle,
                                          AlgInputRequirement* requirements,
                                          int* count);

/**
 * 释放 SDK 在 result 内部分配的 speed_limits[]、signs[] 和 license_plates[]。
 * 对零值 result 调用安全。
 */
ALG_API void AlgFreeResult(AlgResult* result);

/** "alg_sdk.v1.0.0+<backend>"，方便日志/崩溃定位。 */
ALG_API const char* AlgVersion(void);

/** 编译进来的后端名（"xmm" / "rk" / ...）。 */
ALG_API const char* AlgBackendName(void);

ALG_C_END

#endif /* ALG_INTERFACE_H */
