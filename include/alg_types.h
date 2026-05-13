/**
 * @file alg_types.h
 * @brief 算法 SDK 的公共 C ABI 类型定义。
 *
 * SDK 是多模型 + 多后端组合：一个 AlgRun 可能跑完检测→关键点→属性整条链。
 * 因此 AlgResult 是"对象数组"，每个对象用 field_mask 表明带了哪些子结果。
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

/* 原图坐标系下的 2D 框。 */
typedef struct AlgBox_ {
    int   xmin, ymin, xmax, ymax;
    float score;
    int   label; /* 类别 id，单类模型为 0 */
} AlgBox;

/* 关键点集合，xs/ys/scores 都是长度 = count 的数组（scores 可空）。 */
typedef struct AlgKeypoints_ {
    int    count;
    float* xs;
    float* ys;
    float* scores;
} AlgKeypoints;

/* 单个属性条目（性别 / 年龄 / 是否戴口罩 等）。 */
typedef struct AlgAttribute_ {
    char  name[32];
    int   value_int;     /* 离散属性的类别 id（如 gender=1 表示女） */
    float value_float;   /* 连续属性的回归值（如 age=27.3） */
    char  value_str[32]; /* 可读字符串（如 "female"） */
} AlgAttribute;

typedef struct AlgAttributes_ {
    int           count;
    AlgAttribute* items;
} AlgAttributes;

/* Embedding 向量（人脸特征等）。 */
typedef struct AlgEmbedding_ {
    int    dim;
    float* values;
} AlgEmbedding;

/* AlgObject.field_mask 的位标志。 */
typedef enum AlgObjectField_ {
    ALG_FIELD_BOX        = 1 << 0,
    ALG_FIELD_KEYPOINTS  = 1 << 1,
    ALG_FIELD_ATTRIBUTES = 1 << 2,
    ALG_FIELD_EMBEDDING  = 1 << 3,
} AlgObjectField;

/*
 * 一个被检测出来的"目标"。
 *
 * - 纯检测模型：仅 box 字段有效，field_mask = ALG_FIELD_BOX。
 * - 检测+关键点+属性的人脸全套：box | keypoints | attributes 都有效。
 * - 纯分类（无 box）：field_mask = ALG_FIELD_ATTRIBUTES，整个 AlgResult 只有一个 object。
 *
 * 所有 AlgKeypoints* / AlgAttributes* / AlgEmbedding* 由 SDK 持有；
 * 用户通过 AlgFreeResult 统一释放。
 */
typedef struct AlgObject_ {
    int            field_mask;
    AlgBox         box;
    AlgKeypoints*  keypoints;
    AlgAttributes* attributes;
    AlgEmbedding*  embedding;
} AlgObject;

typedef struct AlgResult_ {
    long long  frame_id;
    int        object_count;
    AlgObject* objects;
} AlgResult;

ALG_C_END

#endif /* ALG_TYPES_H */
