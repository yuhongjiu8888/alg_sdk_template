/**
 * @file object.h
 * @brief 内部用的 C++ 对象表示。Solution 在 C++ 侧合并各模型输出后，
 * 最终在 C API 边界由 FillAlgResult() 按 attribute 里的 "category" 标签
 * 分桶到强类型的 AlgTrafficLight[] / AlgSpeedLimit[]。
 */

#ifndef ALG_CORE_OBJECT_H
#define ALG_CORE_OBJECT_H

#include <string>
#include <vector>

#include "alg_types.h"

namespace alg {

/* 内部 field_mask 位标志（仅 SDK 内部使用，不暴露到 C ABI）。 */
enum InternalFieldMask {
    ALG_FIELD_BOX        = 1 << 0,
    ALG_FIELD_ATTRIBUTES = 1 << 1,
};

struct Attribute {
    std::string name;
    int         value_int = 0;
    float       value_float = 0.0f;
    std::string value_str;
};

/* 单个目标的内部表示。
 *
 * - box        : 公共 AlgBox（xyxy + score），是给最终 ABI 的几何 + 置信度
 * - label      : JSON 里 class_names 的下标（0-based）。仅内部用，FillAlgResult
 *                把它 +1 映射成 ABI 里的强类型 enum（0 留给 INVALID）
 * - attributes : 至少包含一条 "category" 条目（"traffic_light" 或 "speed_limit"），
 *                FillAlgResult 据此分桶；二阶段也会带 "class" 条目（信息冗余，便于排错）
 * - drop       : classify_into 标记的「分类置信度低于阈值，最终聚合时跳过」 */
struct Object {
    int                    field_mask = 0;
    AlgBox                 box{};
    int                    label = 0;
    std::vector<Attribute> attributes;
    bool                   drop = false;

    bool has_box()        const { return field_mask & ALG_FIELD_BOX; }
    bool has_attributes() const { return field_mask & ALG_FIELD_ATTRIBUTES; }
};

/**
 * 把 C++ Object 数组按 category attribute 分桶到 AlgResult 的强类型数组。
 * 无 category attribute 的 Object 会被跳过并打 warning。失败返回非 0。
 */
int FillAlgResult(const std::vector<Object>& objs, AlgResult* result);

}  // namespace alg

#endif  // ALG_CORE_OBJECT_H
