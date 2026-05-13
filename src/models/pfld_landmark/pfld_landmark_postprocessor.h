/**
 * @file pfld_landmark_postprocessor.h
 * @brief PFLD 风格人脸关键点回归后处理（N 个点，坐标在 [0, 1] 输入归一化空间）。
 *
 * 示例性实现：网络输出 1 个 1D 张量，长度 = 2 * num_points，前一半 x、后一半 y，
 * 或者 (x0, y0, x1, y1, ...) 交错。通过 JSON 参数 "interleaved" 切换。
 */

#ifndef ALG_MODELS_PFLD_LANDMARK_PFLD_LANDMARK_POSTPROCESSOR_H
#define ALG_MODELS_PFLD_LANDMARK_PFLD_LANDMARK_POSTPROCESSOR_H

#include "core/postprocess/postprocessor.h"

namespace alg {

class PfldLandmarkPostprocessor : public IPostprocessor {
  public:
    Status Configure(const IInferer& inferer, const Json::Value& params) override;
    Status Apply(const IInferer& inferer, const PreprocessState& state,
                 std::vector<Object>* out) override;

  private:
    int  num_points_ = 5;
    bool interleaved_ = true;  /* true: (x0,y0,x1,y1,...) ; false: (x0..xN, y0..yN) */
    bool configured_ = false;
};

}  // namespace alg

#endif  // ALG_MODELS_PFLD_LANDMARK_PFLD_LANDMARK_POSTPROCESSOR_H
