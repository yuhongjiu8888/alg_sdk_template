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

/**
 * 从 JSON 配置创建 SDK 实例。
 *
 * 注意：内部会把 OpenCV 并行框架设为单线程。端侧 OpenCV 只处理 96x96 /
 * 256x64 / ROI 这类小图，pthread 并行的线程池创建与同步开销往往大于收益。
 * 集成方如另有 OpenCV 并行需求，可在本调用之后自行调用 cv::setNumThreads 恢复。
 */
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

/**
 * 设置动态 AIPP 前处理里缩放的执行方，可在 AlgCreate 前或运行期间调用，
 * 便于按实机 CPU 余量动态切换。非法枚举值返回 ALG_E_INVALID_ARG。
 *
 * 只对 Hi3519DV500 生效（见 alg_types.h 里 AlgAippScaleMode 的说明）；
 * 其他后端与 cv610 后端忽略该设置，行为不受影响。
 */
ALG_API AlgStatus AlgSetAippScaleMode(AlgAippScaleMode mode);

/**
 * 限制 AlgRun 的处理帧率（0 = 不限速，默认）。
 *
 * AlgRun 是同步接口：集成方在上一次调用返回前不会再调用，因此在内部睡眠
 * 即可约束实际处理节奏，不依赖集成方用 usleep/硬件节拍自行控帧。适合把
 * 帧率策略随算法配置一起交付。也可在 JSON 的 solution.target_fps 里配置，
 * 本接口用于运行期覆盖。
 *
 * 注意：AlgRun 会阻塞到下一个时间点。如果集成方在调用期间持有硬件帧
 * （如 VPSS 帧未 release），帧队列可能堆积/丢帧；对延迟敏感的集成方式
 * 应在取帧后先拷贝/释放，再调用 AlgRun。
 */
ALG_API AlgStatus AlgSetTargetFps(AlgHandle handle, int fps);

/** "alg_sdk.v3.0.0+<backend>"，方便日志/崩溃定位。 */
ALG_API const char* AlgVersion(void);

/** 编译进来的后端名（"xmm" / "rk" / ...）。 */
ALG_API const char* AlgBackendName(void);

ALG_C_END

#endif /* ALG_INTERFACE_H */
