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
 * 功能:
 *     从 JSON 配置创建并初始化 SDK 实例，加载模型及前后处理参数。
 * 返回值:
 *     ALG_OK（0）表示创建成功；否则返回 AlgStatus 中对应的错误码。
 * 输入:
 *     config_json_path    JSON 配置文件路径，不得为 NULL，不支持内置默认配置。
 * 输出:
 *     handle              句柄变量的地址，不得为 NULL；调用前将句柄变量初始化为 NULL。
 *                         成功时写入实例句柄，失败时不改写；成功创建后须调用 AlgDestroy 释放。
 *
 * 使用 OpenCV 的构建会把其并行框架设为单线程。端侧 OpenCV 只处理 96x96 /
 * 256x64 / ROI 这类小图，pthread 并行的线程池创建与同步开销往往大于收益。
 * 集成方如另有 OpenCV 并行需求，可在本调用之后自行调用 cv::setNumThreads 恢复。
 * 亿智构建使用硬件图像处理，无 OpenCV 依赖。
 */
ALG_API AlgStatus AlgCreate(AlgHandle* handle, const char* config_json_path);

/**
 * 功能:
 *     释放 SDK 实例及其所有资源。
 * 返回值:
 *     ALG_OK（0）表示释放成功；handle 为 NULL 时返回 ALG_E_INVALID_ARG。
 * 输入:
 *     handle    由 AlgCreate 成功创建且尚未销毁的实例句柄。
 * 说明:
 *     调用后句柄失效，调用方应将句柄变量设为 NULL，不得重复销毁或继续使用。
 *     AlgResult 使用内联数组，已返回的结果不受实例销毁影响。
 */
ALG_API AlgStatus AlgDestroy(AlgHandle handle);

/**
 * 功能:
 *     同步处理一帧图像，返回限速牌、禁令 / 停车牌及车牌识别结果。
 * 返回值:
 *     ALG_OK（0）表示处理成功，未检出目标也返回成功；否则返回对应错误码，结果不可使用。
 *     handle、image 或 result 为 NULL 时返回 ALG_E_INVALID_ARG。
 * 输入:
 *     handle    有效的 SDK 实例句柄。
 *     image     图像描述指针，不得为 NULL；字段要求见 AlgImage。
 *               图像缓冲由调用方持有，在函数返回前须保持有效且不被修改。
 * 输出:
 *     result    调用方提供的结果结构指针，不得为 NULL，建议零初始化。
 *               成功时覆盖结果数量、本帧有效元素及 frame_id；仅数量范围内的元素有效。
 *               结果无需额外释放，可复用该结构接收下一帧结果。
 */
ALG_API AlgStatus AlgRun(AlgHandle handle, const AlgImage* image, AlgResult* result);

/**
 * 功能:
 *     动态设置 SDK 全局日志等级，可在 AlgCreate 前或运行期间调用，对所有实例生效。
 * 返回值:
 *     ALG_OK（0）表示设置成功；非法枚举值返回 ALG_E_INVALID_ARG，当前等级不变。
 * 输入:
 *     level    日志等级，取值见 AlgLogLevel；标准构建默认等级为 ALG_LOG_WARN。
 */
ALG_API AlgStatus AlgSetLogLevel(AlgLogLevel level);

/**
 * 功能:
 *     设置动态 AIPP 前处理中缩放的执行方，可在 AlgCreate 前或运行期间调用，对所有实例生效。
 * 返回值:
 *     ALG_OK（0）表示参数合法；非法枚举值返回 ALG_E_INVALID_ARG，当前模式不变。
 * 输入:
 *     mode    缩放模式，取值见 AlgAippScaleMode，默认为 ALG_AIPP_SCALE_AUTO。
 *
 * 只对 Hi3519DV500 生效（见 alg_types.h 里 AlgAippScaleMode 的说明）；
 * 其他后端与 cv610 后端忽略该设置，行为不受影响。非海思构建保留本兼容入口，
 * 校验参数后直接返回，不编入 AIPP 模块。
 */
ALG_API AlgStatus AlgSetAippScaleMode(AlgAippScaleMode mode);

/**
 * 功能:
 *     设置指定实例的 AlgRun 处理帧率上限，覆盖 JSON 中的 solution.target_fps。
 * 返回值:
 *     ALG_OK（0）表示设置成功；handle 为 NULL 或 fps 超出范围时返回 ALG_E_INVALID_ARG。
 * 输入:
 *     handle    有效的 SDK 实例句柄。
 *     fps       帧率上限，单位为帧/秒，范围为 [0, 1000]；0 表示不限速。
 *               未调用本接口时使用 JSON 配置值，未配置时默认为 0。
 *
 * 说明:
 *     AlgRun 通过内部等待限制处理节奏，每次设置后重新计算下一帧时间点。
 *     若调用期间持有 VPSS 等硬件帧，等待可能导致帧队列堆积/丢帧；
 *     对延迟敏感的调用方应在取帧后先拷贝并释放硬件帧，再调用 AlgRun。
 */
ALG_API AlgStatus AlgSetTargetFps(AlgHandle handle, int fps);

/**
 * 功能:
 *     获取 SDK 版本信息，无需创建实例即可调用。
 * 返回值:
 *     版本字符串指针，格式为 "alg_sdk.v3.0.0+<backend>"。
 *     字符串由 SDK 持有，调用方不得修改或释放，动态库卸载后指针失效。
 * 输入:
 *     无。
 */
ALG_API const char* AlgVersion(void);

/**
 * 功能:
 *     获取编译进 SDK 的推理后端名称，无需创建实例即可调用。
 * 返回值:
 *     后端名称字符串指针，取值为 "xmm"、"svp_acl"、"ez" 或 "mnn"。
 *     字符串由 SDK 持有，调用方不得修改或释放，动态库卸载后指针失效。
 * 输入:
 *     无。
 */
ALG_API const char* AlgBackendName(void);

ALG_C_END

#endif /* ALG_INTERFACE_H */
