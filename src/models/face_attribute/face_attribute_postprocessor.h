/**
 * @file face_attribute_postprocessor.h
 * @brief 多任务人脸属性分类后处理：每个属性是一个独立的 softmax/sigmoid 头。
 *
 * JSON 参数示例：
 *   {
 *     "type": "face_attribute",
 *     "labels": [
 *       { "name": "gender", "output_index": 0, "kind": "softmax",  "classes": ["male", "female"] },
 *       { "name": "mask",   "output_index": 1, "kind": "sigmoid",  "threshold": 0.5 },
 *       { "name": "age",    "output_index": 2, "kind": "regress" }
 *     ]
 *   }
 */

#ifndef ALG_MODELS_FACE_ATTRIBUTE_FACE_ATTRIBUTE_POSTPROCESSOR_H
#define ALG_MODELS_FACE_ATTRIBUTE_FACE_ATTRIBUTE_POSTPROCESSOR_H

#include <string>
#include <vector>

#include "core/postprocess/postprocessor.h"

namespace alg {

class FaceAttributePostprocessor : public IPostprocessor {
  public:
    Status Configure(const IInferer& inferer, const Json::Value& params) override;
    Status Apply(const IInferer& inferer, const PreprocessState& state,
                 std::vector<Object>* out) override;

  private:
    enum class HeadKind { kSoftmax, kSigmoid, kRegress };
    struct Head {
        std::string              name;
        int                      output_index = 0;
        HeadKind                 kind = HeadKind::kSoftmax;
        std::vector<std::string> classes;
        float                    threshold = 0.5f;
    };
    std::vector<Head> heads_;
    bool              configured_ = false;
};

}  // namespace alg

#endif  // ALG_MODELS_FACE_ATTRIBUTE_FACE_ATTRIBUTE_POSTPROCESSOR_H
