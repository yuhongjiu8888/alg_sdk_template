/**
 * @file chain_solution.h
 * @brief 通用业务编排：按 JSON stages 顺序串多个 ModelInstance。
 *
 * 性能契约：
 *  - stage 之间的引用在 Init 阶段就解析成数组索引，运行期不再做 hash 查找。
 *  - decoded_bgr_、stage_produces_ 都挂成员；运行期只 clear() 不重分配。
 *  - 没有任何 ROI 子模型时（纯单模型 solution），完全跳过原图解码。
 */

#ifndef ALG_CORE_SOLUTION_CHAIN_SOLUTION_H
#define ALG_CORE_SOLUTION_CHAIN_SOLUTION_H

#include <memory>
#include <string>
#include <vector>

#include <opencv2/core/mat.hpp>

#include "alg_types.h"
#include "core/config/config.h"
#include "core/instance/model_instance.h"
#include "core/object.h"
#include "core/status.h"

namespace alg {

class ChainSolution {
  public:
    Status Init(const SolutionConfig& cfg);
    Status Run(const AlgImage& image, std::vector<Object>* out_objects);

  private:
    struct StageProduce {
        std::vector<Object> objects;
    };

    /* Init 期把 JSON 里按名字的引用解析掉，运行期完全走索引。 */
    struct RuntimeStage {
        StageConfig     cfg;            /* 拷贝一份，运行期只读 */
        ModelInstance*  model;          /* 直接指针，省 unordered_map 查询 */
        int             input_idx;      /* -1 表示 input == "image" */
        int             output_target;  /* -1 表示 produces == "objects" */
    };

    std::vector<std::unique_ptr<ModelInstance>>  model_storage_;  /* 持有所有模型实例 */
    std::vector<RuntimeStage>                    stages_;
    std::vector<StageProduce>                    stage_produces_; /* 与 stages_ 索引对齐 */
    cv::Mat                                      decoded_bgr_;    /* 复用：避免每帧重新分配 */
    bool                                         needs_decoded_bgr_ = false;
    bool                                         initialized_ = false;

    Status RunStage(int stage_idx, const AlgImage& image, const cv::Mat& decoded_bgr);
};

}  // namespace alg

#endif  // ALG_CORE_SOLUTION_CHAIN_SOLUTION_H
