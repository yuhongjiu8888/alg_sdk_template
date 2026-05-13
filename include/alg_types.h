/**
 * @file alg_types.h
 * @brief 算法 SDK 的公共 C ABI 类型定义。
 *
 * 本分支聚焦红绿灯检测 + 巴西限速牌识别，两个落地模型的输出形态都是
 * 「检测框 + 可选属性」。AlgObject 用 field_mask 标明每个对象带了哪些子结果，
 * 同一份 ABI 既能跑「只检测」也能跑「检测+识别」。
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
    int            stride;
    int            data_len;
    const void*    data;
} AlgImage;

/* 原图坐标系下的 2D 框。
 * 二阶段链路（限速牌）里 box.label 会被分类器覆盖成最终类别 idx，
 * box.score 会被乘以分类置信度作为联合置信度。 */
typedef struct AlgBox_ {
    int   xmin, ymin, xmax, ymax;
    float score;
    int   label;
} AlgBox;

/* 单个属性条目。本分支两种产出方式：
 *   - { name="class",    value_int=类别 idx,     value_str=类名 (red_light / 60 / ...) }
 *   - { name="category", value_str="traffic_light" 或 "speed_limit" } */
typedef struct AlgAttribute_ {
    char  name[32];
    int   value_int;
    float value_float;
    char  value_str[32];
} AlgAttribute;

typedef struct AlgAttributes_ {
    int           count;
    AlgAttribute* items;
} AlgAttributes;

/* AlgObject.field_mask 的位标志。 */
typedef enum AlgObjectField_ {
    ALG_FIELD_BOX        = 1 << 0,
    ALG_FIELD_ATTRIBUTES = 1 << 1,
} AlgObjectField;

/*
 * 一个被检测出来的目标。
 *
 * - 红绿灯单阶段：仅 box 有效，attributes 可选（如 JSON 配了 class_names / category 就会带上）。
 * - 限速牌二阶段：box + attributes 都有效；attributes[0] 为 class，[1] 为 category。
 *
 * AlgAttributes* 由 SDK 持有；用户通过 AlgFreeResult 一次性释放。
 */
typedef struct AlgObject_ {
    int            field_mask;
    AlgBox         box;
    AlgAttributes* attributes;
} AlgObject;

typedef struct AlgResult_ {
    long long  frame_id;
    int        object_count;
    AlgObject* objects;
} AlgResult;

ALG_C_END

#endif /* ALG_TYPES_H */
