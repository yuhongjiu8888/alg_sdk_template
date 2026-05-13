/**
 * @file postprocessor.h
 * @brief 模型后处理基类。每个模型类型派生一个实现（FCOS / PFLD / 分类网...）。
 *
 * 关键区别于旧版本：
 *   1) Configure 接收 JSON 节点 → 阈值/步长/类目数都从配置文件来；
 *   2) Apply 输出 std::vector<Object>，而不是直接写 AlgResult，
 *      由 Solution 层决定每个 Object 的字段怎么并入最终结果。
 */

#ifndef ALG_CORE_POSTPROCESS_POSTPROCESSOR_H
#define ALG_CORE_POSTPROCESS_POSTPROCESSOR_H

#include <json/json.h>

#include <vector>

#include "alg_types.h"
#include "core/infer/inferer.h"
#include "core/object.h"
#include "core/preprocess/preprocessor.h"
#include "core/status.h"

namespace alg {

class IPostprocessor {
  public:
    virtual ~IPostprocessor() = default;

    /**
     * 一次性初始化：检查输出张量是否符合预期 + 读 JSON 参数。
     * params 是配置文件里这块模型的 "postprocess" 节点本身（含 "type" 字段）。
     */
    virtual Status Configure(const IInferer& inferer, const Json::Value& params) = 0;

    /**
     * 解码本次 Forward 后的输出。`state` 给的是相对于"喂给本模型的那张图"
     * 的几何变换，用于把坐标映射回那张图。后续 Solution 层会再映射回原帧。
     */
    virtual Status Apply(const IInferer& inferer, const PreprocessState& state,
                         std::vector<Object>* out) = 0;
};

}  // namespace alg

#endif  // ALG_CORE_POSTPROCESS_POSTPROCESSOR_H
