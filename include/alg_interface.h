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
 * 动态设置 SDK 全局日志等级，可在 AlgCreate 前或运行期间调用。
 * 标准构建默认等级为 ALG_LOG_WARN；传入非法枚举值返回 ALG_E_INVALID_ARG。
 */
ALG_API AlgStatus AlgSetLogLevel(AlgLogLevel level);

/** "alg_sdk.v3.0.0+<backend>"，方便日志/崩溃定位。 */
ALG_API const char* AlgVersion(void);

/** 编译进来的后端名（"xmm" / "rk" / ...）。 */
ALG_API const char* AlgBackendName(void);

ALG_C_END

#endif /* ALG_INTERFACE_H */
