/**
 * @file alg_types.h
 * @brief alg_sdk 公共 C ABI 类型定义。
 *
 * 本分支聚焦巴西限速牌 / 停车牌识别 + 车牌识别落地模型。AlgResult 用强类型字段
 * 直接表达每个检测器的产出（speed_limits[] / signs[] / license_plates[]），应用
 * 代码不需要查 attribute 字符串，也不依赖 JSON 里 class_names 顺序之外的约定。
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

/* SDK 全局日志等级。数值越大输出越详细；OFF 关闭全部 SDK 日志。 */
typedef enum AlgLogLevel_ {
    ALG_LOG_OFF   = -1,
    ALG_LOG_ERROR = 0,
    ALG_LOG_WARN  = 1,
    ALG_LOG_INFO  = 2,
    ALG_LOG_DEBUG = 3,
} AlgLogLevel;

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
 *   - 限速牌：联合置信度 = 检测置信度 × OCR 分类置信度
 *   - 停车/禁令牌：联合置信度
 *   - 车牌：联合置信度 = 检测置信度 × 识别置信度
 * 应用统一只看这一个分数即可。 */
typedef struct AlgBox_ {
    int   xmin, ymin, xmax, ymax;
    float score;
} AlgBox;

/* ============================================================== */
/*                   限速牌识别 (speed limit sign)                  */
/* ============================================================== */

/* 限速牌类别。枚举编号即 km/h ÷ 10（SLV_10=1→10km/h, ..., SLV_120=12→120km/h），
 * 限速值都是 10 的倍数，故 value × 10 == km/h，应用可直接换算或 switch / 查表。
 *
 * value 由后处理器从 cls_cfg.postprocess.class_names 推导（解析类名数值 ÷ 10），
 * 不依赖 class_names 的排列顺序（OCR 的 class_names 可能是"追加排序"而非数值序）。
 * 0 留作 INVALID。 */
typedef enum AlgSpeedLimitValue_ {
    SLV_INVALID = 0,    /* 二阶段分类置信度 < 阈值 / 字符组合非法的框已由 SDK 内部 drop，正常不出现 */
    SLV_10      = 1,
    SLV_20      = 2,
    SLV_30      = 3,
    SLV_40      = 4,
    SLV_50      = 5,
    SLV_60      = 6,
    SLV_70      = 7,
    SLV_80      = 8,
    SLV_90      = 9,
    SLV_100     = 10,
    SLV_110     = 11,
    SLV_120     = 12,
} AlgSpeedLimitValue;

/* 单个限速牌识别结果。 */
typedef struct AlgSpeedLimit_ {
    AlgBox              box;          /* 原图坐标系下的框，box.score = det × cls 联合置信度 */
    AlgSpeedLimitValue  value;        /* 限速值类别（enum），编号即隐含 km/h */
} AlgSpeedLimit;

/* ============================================================== */
/*                  禁令 / 停车牌 (prohibition sign)               */
/* ============================================================== */

/* 非限速的牌种（不带可读数字，无法用 AlgSpeedLimitValue 表达）。
 * 判别轴 = 低分辨率下能否与限速牌分形状：
 *   - PARE 八边形：Stage1 检测终端类，直接输出（box.score = 检测置信度）
 *   - NO_PARKING 红圈✕：Stage2 门控第 3 路（box.score = det × P(禁停) 联合置信度）
 * 0 留作 INVALID。扩展新牌种在尾部追加枚举即可，不影响限速 value 语义。 */
typedef enum AlgSignType_ {
    SIGN_INVALID    = 0,
    SIGN_NO_PARKING = 1,    /* R-6c 禁止停车（红圈 + 黑 E + 红色 ✕） */
    SIGN_PARE       = 2,    /* 停车让行（巴西 R-1，红色八边形，等同 STOP） */
} AlgSignType;

/* 单个禁令 / 停车牌识别结果。 */
typedef struct AlgSign_ {
    AlgBox       box;    /* 原图坐标系下的框，box.score = 联合置信度 */
    AlgSignType  type;
} AlgSign;

/* ============================================================== */
/*                      车牌识别 (license plate)                    */
/* ============================================================== */

/* 单个车牌识别结果。
 *   box.score = det × rec 联合置信度（与限速牌一致，应用统一只看这一个分数）
 *   text      = 识别文本（字符集 '0'-'9','A'-'Z'，无分隔符，如 "ABC1D23"；
 *               未识别到内容时为空串）
 *   rec_score = 识别单独置信度（CTC greedy 被保留字符概率之积，排错/二次过滤用） */
typedef struct AlgLicensePlate_ {
    AlgBox  box;
    char    text[32];    /* 理论最长 = LPRNet 时间步数(32) */
    float   rec_score;
} AlgLicensePlate;

/* 固定容量与检测阶段 max_det 保持一致；结果直接内联在 AlgResult 中，不做堆分配。 */
#define ALG_MAX_SPEED_LIMIT_RESULTS 5
#define ALG_MAX_SIGN_RESULTS 5
#define ALG_MAX_LICENSE_PLATE_RESULTS 2

/* ============================================================== */
/*                          完整结果                                */
/* ============================================================== */

/* 一帧的算法输出。
 *
 *  - 单 solution 跑限速牌 (speed_limit.json)：speed_limits / signs 可能非空
 *    （signs = PARE / 禁止停车等非限速牌种）。
 *  - 单 solution 跑车牌 (license_plate.json)：license_plates 可能非空。
 *
 * 数组内联在结构体中。调用方可把 AlgResult 放在栈上，无需额外释放；下一帧调用
 * 直接覆盖数量和有效元素。 */
typedef struct AlgResult_ {
    long long           frame_id;

    int                 speed_limit_count;
    AlgSpeedLimit       speed_limits[ALG_MAX_SPEED_LIMIT_RESULTS];

    int                 sign_count;      /* 禁令/停车牌（PARE / 禁止停车） */
    AlgSign             signs[ALG_MAX_SIGN_RESULTS];

    int                 license_plate_count;
    AlgLicensePlate     license_plates[ALG_MAX_LICENSE_PLATE_RESULTS];
} AlgResult;

ALG_C_END

#endif /* ALG_TYPES_H */
