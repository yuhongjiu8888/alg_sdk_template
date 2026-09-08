/**
 * @file model_instance.h
 * @brief 单个网络的 pre → infer → post 三件套。
 *
 * 多个 ModelInstance 由 Solution 组合成业务流水线。每个 ModelInstance 持有
 * 一个 IInferer（独享芯片资源）+ 一个通用 LetterboxPreprocessor + 一个
 * 模型类型对应的 IPostprocessor。所有三件套的参数都来自一份 JSON 节点。
 */

#ifndef ALG_CORE_INSTANCE_MODEL_INSTANCE_H
#define ALG_CORE_INSTANCE_MODEL_INSTANCE_H

#include <memory>
#include <string>
#include <vector>

#include "alg_types.h"
#include "core/config/config.h"
#include "core/infer/inferer.h"
#include "core/object.h"
#include "core/postprocess/postprocessor.h"
#include "core/preprocess/preprocessor.h"
#include "core/status.h"

namespace alg {

class ModelInstance {
  public:
    ModelInstance();
    ~ModelInstance();

    /** 用 ModelInstanceConfig 装配 inferer + preprocessor + postprocessor。 */
    Status Init(const ModelInstanceConfig& cfg);

    /** 对单张 AlgImage 跑一次完整三件套。 */
    Status Run(const AlgImage& image, std::vector<Object>* out);

    /** 图像已由 VPSS 按本模型的几何配置处理到网络输入尺寸。 */
    Status RunPrepared(const AlgImage& image, int original_width, int original_height,
                       std::vector<Object>* out);

    const std::string& name() const { return cfg_.name; }
    const PreprocessConfig& pre_cfg() const { return cfg_.pre; }
    int source_id() const { return cfg_.pre.source_id; }

  private:
    ModelInstanceConfig             cfg_;
    std::unique_ptr<IInferer>       inferer_;
    std::unique_ptr<IPreprocessor>  pre_;
    std::unique_ptr<IPreprocessor>  prepared_pre_;
    std::unique_ptr<IPostprocessor> post_;
    bool                            initialized_ = false;

    Status RunImpl(const AlgImage& image, bool geometry_prepared,
                   int original_width, int original_height,
                   std::vector<Object>* out);
};

}  // namespace alg

#endif  // ALG_CORE_INSTANCE_MODEL_INSTANCE_H
