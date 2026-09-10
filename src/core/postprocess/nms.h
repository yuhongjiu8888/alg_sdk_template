/**
 * @file nms.h
 * @brief 通用 NMS 工具 —— 检测模型复用。
 *
 * 性能：
 *  - scratch 参数可由调用方持有，避免每帧分配 suppressed mask。
 *  - 用 uint8_t mask 而非 vector<bool>，避开位打包的 proxy reference 开销。
 */

#ifndef ALG_CORE_POSTPROCESS_NMS_H
#define ALG_CORE_POSTPROCESS_NMS_H

#include <cstdint>
#include <vector>

namespace alg {

struct Proposal {
    float x1, y1, x2, y2;
    float score;
    int   label = 0;
    float area = 0;
};

/* 调用方可持有的可复用 scratch 缓冲；clear() 保留 capacity。 */
struct NmsScratch {
    std::vector<uint8_t> suppressed;
};

/**
 * 就地硬 NMS。FCOS 风格 IoU（无 +1）。
 * scratch 不为空则复用其 suppressed 缓冲；为空则函数内部临时分配（一次性场合）。
 * max_keep > 0 时，按分数保留到该数量后立即停止；结果与完整 NMS 后 resize 等价。
 */
void Nms(std::vector<Proposal>& props, float iou_thresh,
         NmsScratch* scratch = nullptr, int max_keep = 0);

}  // namespace alg

#endif  // ALG_CORE_POSTPROCESS_NMS_H
