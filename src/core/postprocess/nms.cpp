#include "core/postprocess/nms.h"

#include <algorithm>

namespace alg {

void Nms(std::vector<Proposal>& props, float iou_thresh, NmsScratch* scratch) {
    if (props.empty()) return;

    for (auto& p : props) p.area = (p.x2 - p.x1) * (p.y2 - p.y1);

    std::sort(props.begin(), props.end(),
              [](const Proposal& a, const Proposal& b) { return a.score > b.score; });

    const size_t n = props.size();

    /* suppressed mask 复用：scratch 提供时仅 resize（capacity 已够时零分配）。 */
    NmsScratch local;
    NmsScratch& sc = scratch ? *scratch : local;
    sc.suppressed.assign(n, 0);  /* assign 比 resize+fill 在 capacity 足够时更优 */

    uint8_t* sup = sc.suppressed.data();
    Proposal* pp = props.data();

    for (size_t i = 0; i < n; ++i) {
        if (sup[i]) continue;
        const Proposal& a = pp[i];
        for (size_t j = i + 1; j < n; ++j) {
            if (sup[j]) continue;
            const Proposal& b = pp[j];
            if (a.label != b.label) continue;

            float xx1 = a.x1 > b.x1 ? a.x1 : b.x1;
            float yy1 = a.y1 > b.y1 ? a.y1 : b.y1;
            float xx2 = a.x2 < b.x2 ? a.x2 : b.x2;
            float yy2 = a.y2 < b.y2 ? a.y2 : b.y2;
            float iw = xx2 - xx1;
            float ih = yy2 - yy1;
            if (iw <= 0 || ih <= 0) continue;
            float inter = iw * ih;
            float uni = a.area + b.area - inter;
            /* inter / uni > th  ⇔  inter > th * uni  （省一次除法） */
            if (inter > iou_thresh * uni) sup[j] = 1;
        }
    }

    size_t w = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!sup[i]) {
            if (w != i) pp[w] = pp[i];
            ++w;
        }
    }
    props.resize(w);
}

}  // namespace alg
