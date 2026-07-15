/**
 * @file config.h
 * @brief 从 JSON 文件解析出来的、纯数据的配置结构体。
 *
 * 解析层与使用层分离：core 内部不感知 jsoncpp 头文件，只看这些 POD 结构体。
 */

#ifndef ALG_CORE_CONFIG_CONFIG_H
#define ALG_CORE_CONFIG_CONFIG_H

#include <json/json.h>

#include <map>
#include <string>
#include <vector>

#include "alg_types.h"
#include "core/preprocess/preprocessor.h"

namespace alg {

/* 单个模型实例的配置：模型路径 + 前处理 + 后处理类型与参数。 */
struct ModelInstanceConfig {
    std::string      name;            /* JSON 里的 key，便于排错 */
    std::string      model_path;
    PreprocessConfig pre;
    std::string      post_type;       /* 后处理类型名（向 PostprocessorRegistry 查询） */
    Json::Value      post_params;     /* 后处理类型自行解读的参数节点 */
};

/* Stage 输入来源类型。 */
enum class StageInputKind {
    kImage,                  /* 原始输入帧 */
    kObjectsFromStage,       /* 在另一个 stage 输出的 objects 上逐个跑 */
};

/* Stage 输出落地类型。 */
enum class StageOutputKind {
    kCreateObjects,    /* 本 stage 的产物作为 top-level objects（典型：detector） */
    kFillAttributes,   /* 本 stage 的产物填到 target_stage 已产 objects 的 attributes 字段 */
    kClassifyInto,     /* 分类器：合并 attributes，把 sub.label 写回 src.label，
                          src.box.score 乘以 sub.box.score（联合置信度）；
                          子模型若返回空（典型：joint_conf 低于阈值）则丢弃 src 框。
                          典型用法：detector → ROI classifier 二阶段，把识别置信度低的框过滤掉。 */
    kKeypointsInto,    /* 关键点回归：把 sub.keypoints 写回 src.keypoints（内部预留） */
    kMaskInto,         /* 分割掩码：把 sub.mask 写回 src.mask（内部预留） */
};

struct CropConfig {
    float expand_ratio = 1.0f;  /* 在 box 周围按比例扩边再裁剪 */
    bool  square = false;       /* 是否扩为正方形 */
    int   pad_value = -1;       /* >=0：扩边框越界处用此灰度值填充（等价训练端 warpAffine
                                   borderValue），保正方不形变；-1（默认）：旧行为 clamp 到图内 */
};

/* 固定 ROI 区域：在原图上按像素坐标裁剪后再送模型检测。 */
struct RoiConfig {
    bool enabled = false;
    int x = 0;       /* 左上角 x（像素） */
    int y = 0;       /* 左上角 y（像素） */
    int width = 0;   /* 裁剪宽度 */
    int height = 0;  /* 裁剪高度 */
};

/* v3.5 透传规则：上游检测的某个 label 是"终端类"（如 Stage1 的 pare 八边形），
 * 不进本 classify 阶段的子模型，直接打上 category/牌种透出。
 * 用途：pare 走 Stage1 检测直出，不该被 OCR 分类器当非限速 drop 掉。 */
struct PassthroughRule {
    int         label = -1;          /* 上游 object.label（检测类 idx） */
    std::string category;            /* 透出的 category（FillAlgResult 据此分桶，如 "pare"） */
    int         sign_value = 0;      /* 写入 object.value（如 AlgSignType SIGN_PARE） */
    float       min_score = 0.0f;    /* 检测分低于此值则 drop（0=不额外过滤，pare 弱类可设 0.85） */
};

struct StageConfig {
    std::string     name;
    std::string     model_ref;       /* 引用 ModelInstanceConfig.name */
    StageInputKind  input_kind;
    std::string     input_stage;     /* input_kind == kObjectsFromStage 时引用的 stage */
    CropConfig      crop;
    RoiConfig       roi;
    StageOutputKind output_kind;
    std::string     output_target;   /* output_kind != kCreateObjects 时引用的 stage */
    float           score_threshold = 0.0f;  /* classify_into：合并后联合分(det×cls)低于此值则 drop；
                                                 0（默认）= 不过滤。卡的是各阶段阈值卡不到的两阶段乘积。 */
    std::vector<PassthroughRule> passthrough;          /* 终端类透传（按上游 label 命中） */
    std::map<std::string, int>   min_box_short;        /* category → 最小框短边(px)；小于则 drop
                                                          （nopark_min_size：R-6c 禁停仅近处大牌生效） */
};

/* 整份 JSON 配置。 */
struct SolutionConfig {
    std::string                       solution_type;   /* 目前只支持 "chain" */
    std::vector<ModelInstanceConfig>  models;
    std::vector<StageConfig>          stages;
};

/* 从 JSON 文件解析；失败返回非 ALG_OK 并把错误写到 err_msg。 */
AlgStatus LoadSolutionConfig(const std::string& json_path, SolutionConfig* out,
                             std::string* err_msg = nullptr);

}  // namespace alg

#endif  // ALG_CORE_CONFIG_CONFIG_H
