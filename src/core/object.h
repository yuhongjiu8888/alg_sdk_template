/**
 * @file object.h
 * @brief 内部用的 C++ 对象表示。Solution 在 C++ 侧合并各模型输出后，
 * 最终在 C API 边界一次性翻译成 AlgObject。
 */

#ifndef ALG_CORE_OBJECT_H
#define ALG_CORE_OBJECT_H

#include <string>
#include <vector>

#include "alg_types.h"

namespace alg {

struct Attribute {
    std::string name;
    int         value_int = 0;
    float       value_float = 0.0f;
    std::string value_str;
};

/* 单个目标，内部表示，对应 C ABI 的 AlgObject。 */
struct Object {
    int                    field_mask = 0;
    AlgBox                 box{};
    std::vector<Attribute> attributes;

    /* 内部标记：被 classify_into stage 判定应丢弃。
     * 不暴露到 C ABI，仅 ChainSolution 在最终聚合时跳过。 */
    bool                   drop = false;

    bool has_box()        const { return field_mask & ALG_FIELD_BOX; }
    bool has_attributes() const { return field_mask & ALG_FIELD_ATTRIBUTES; }
};

/**
 * 把 C++ Object 数组翻译成 C ABI 的 AlgResult.objects。
 * 失败返回非 0 并保证不泄漏。
 */
int FillAlgResult(const std::vector<Object>& objs, AlgResult* result);

}  // namespace alg

#endif  // ALG_CORE_OBJECT_H
