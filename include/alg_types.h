/**
 * @file alg_types.h
 * @brief alg_sdk 公共 C ABI 类型定义。
 *
 * 本分支聚焦红绿灯检测 + 巴西限速牌识别两个落地模型。AlgResult 用强类型字段
 * 直接表达每个检测器的产出（traffic_lights[] / speed_limits[]），应用代码
 * 不需要查 attribute 字符串，也不依赖 JSON 里 class_names 顺序之外的约定。
 */

#ifndef ALG_TYPES_H
#define ALG_TYPES_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) || defined(_WIN64)
#define ALG_API __declspec(dllexport)
#else
#define ALG_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
#define ALG_C_BEGIN extern "C" {
#define ALG_C_END }
#else
#define ALG_C_BEGIN
#define ALG_C_END
#endif

ALG_C_BEGIN

/* ============================================================== */
/*                          基础类型                                */
/* ============================================================== */

/* 不透明 SDK 句柄。 */
typedef struct AlgContext* AlgHandle;

/* 返回码：0 = 成功，负数 = 错误。 */
typedef enum AlgStatus_ {
    ALG_OK = 0,
    ALG_E_INVALID_ARG = -1,
    ALG_E_NOT_INITIALIZED = -2,
    ALG_E_BACKEND = -3,         /* 芯片后端错误 */
    ALG_E_MODEL_NOT_FOUND = -4, /* 后处理类型未注册 */
    ALG_E_PREPROCESS = -5,
    ALG_E_POSTPROCESS = -6,
    ALG_E_OOM = -7,
    ALG_E_IO = -8,
    ALG_E_CONFIG = -9,          /* JSON 配置文件解析失败 */
    ALG_E_UNKNOWN = -99,
} AlgStatus;

/* 输入像素格式。 */
typedef enum AlgPixelFormat_ {
    ALG_PIX_BGR  = 0,
    ALG_PIX_RGB  = 1,
    ALG_PIX_GRAY = 2,
    ALG_PIX_NV12 = 3,
    ALG_PIX_NV21 = 4,
} AlgPixelFormat;

/* 输入图像描述，data 由调用方持有。 */
typedef struct AlgImage_ {
    AlgPixelFormat format;
    int            width;
    int            height;
    int            stride;     /* 每行像素跨度（字节）；0 表示按 width * bpp 推算 */
    int            data_len;   /* 像素数据字节数 */
    const void*    data;       /* 像素数据缓冲区 */
} AlgImage;

/* 原图坐标系下的 2D 检测框。
 * score 含义：
 *   - 红绿灯：检测置信度
 *   - 限速牌：联合置信度 = 检测置信度 × 双头分类置信度
 * 应用统一只看这一个分数即可。 */
typedef struct AlgBox_ {
    int   xmin, ymin, xmax, ymax;
    float score;
} AlgBox;

/* ============================================================== */
/*                     红绿灯检测 (traffic light)                   */
/* ============================================================== */

/* 红绿灯颜色类别。
 *
 * 枚举值 = JSON 中 tld_cfg.postprocess.class_names[label] 的下标 + 1，0 留作 INVALID。
 * 顺序约定：class_names 必须按
 *     ["red_light", "yellow_light", "green_light", "off_light"]
 * 排列，否则颜色映射会错位。 */
typedef enum AlgTrafficLightColor_ {
    TLC_INVALID = 0,    /* 检测到了但分类异常（正常不出现） */
    TLC_RED     = 1,    /* 红灯 */
    TLC_YELLOW  = 2,    /* 黄灯 */
    TLC_GREEN   = 3,    /* 绿灯 */
    TLC_OFF     = 4,    /* 熄灭 */
} AlgTrafficLightColor;

/* 单个红绿灯检测结果。 */
typedef struct AlgTrafficLight_ {
    AlgBox                box;     /* 原图坐标系下的框，box.score = 检测置信度 */
    AlgTrafficLightColor  color;
} AlgTrafficLight;

/* ============================================================== */
/*                   限速牌识别 (speed limit sign)                  */
/* ============================================================== */

/* 限速牌类别。value_kmh 为对应实际数值（km/h），便于直接打印 / 上报。
 *
 * 枚举值 = JSON 中 cls_cfg.postprocess.class_names[label] 的下标 + 1，0 留作 INVALID。
 * 顺序约定：class_names 必须按
 *     ["10","20","30","40","50","60","70","80","100"]
 * 排列，否则 value 与 value_kmh 会错位。 */
typedef enum AlgSpeedLimitValue_ {
    SLV_INVALID = 0,    /* 二阶段分类置信度 < 阈值的框已由 SDK 内部 drop，正常不出现 */
    SLV_10      = 1,
    SLV_20      = 2,
    SLV_30      = 3,
    SLV_40      = 4,
    SLV_50      = 5,
    SLV_60      = 6,
    SLV_70      = 7,
    SLV_80      = 8,
    SLV_100     = 9,
} AlgSpeedLimitValue;

/* 单个限速牌识别结果。 */
typedef struct AlgSpeedLimit_ {
    AlgBox              box;          /* 原图坐标系下的框，box.score = det × cls 联合置信度 */
    AlgSpeedLimitValue  value;        /* 限速值类别（enum） */
    int                 value_kmh;    /* 限速值 km/h（10/20/.../100），SLV_INVALID 时为 0 */
} AlgSpeedLimit;

/* ============================================================== */
/*                          完整结果                                */
/* ============================================================== */

/* 一帧的算法输出。
 *
 *  - 单 solution 跑红绿灯 (traffic_light.json)：只有 traffic_lights 非空。
 *  - 单 solution 跑限速牌 (speed_limit.json)：  只有 speed_limits 非空。
 *  - 二合一 (all.json)：两个数组都可能非空。
 *
 * 两个数组由 SDK 持有；应用通过 AlgFreeResult 一次性释放。 */
typedef struct AlgResult_ {
    long long           frame_id;

    int                 traffic_light_count;
    AlgTrafficLight*    traffic_lights;

    int                 speed_limit_count;
    AlgSpeedLimit*      speed_limits;
} AlgResult;

ALG_C_END

#endif /* ALG_TYPES_H */
