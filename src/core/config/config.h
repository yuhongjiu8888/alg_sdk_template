/**
 * @file config.h
 * @brief 从 JSON 文件解析出来的、纯数据的配置结构体。
 *
 * 解析层与使用层分离：core 内部不感知 jsoncpp 头文件，只看这些 POD 结构体。
 */

#ifndef ALG_CORE_CONFIG_CONFIG_H
#define ALG_CORE_CONFIG_CONFIG_H

#include <json/json.h>

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
};

struct CropConfig {
    float expand_ratio = 1.0f;  /* 在 box 周围按比例扩边再裁剪 */
    bool  square = false;       /* 是否扩为正方形 */
};

struct StageConfig {
    std::string     name;
    std::string     model_ref;       /* 引用 ModelInstanceConfig.name */
    StageInputKind  input_kind;
    std::string     input_stage;     /* input_kind == kObjectsFromStage 时引用的 stage */
    CropConfig      crop;
    StageOutputKind output_kind;
    std::string     output_target;   /* output_kind != kCreateObjects 时引用的 stage */
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
