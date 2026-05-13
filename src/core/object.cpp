#include "core/object.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include "alg_interface.h"
#include "core/logger.h"

namespace alg {

namespace {

/* 在 attributes 里找指定 name 的条目；找不到返回空。 */
const Attribute* FindAttr(const std::vector<Attribute>& attrs, const char* name) {
    for (const auto& a : attrs) {
        if (a.name == name) return &a;
    }
    return nullptr;
}

/* 把内部 box 拷到 ABI box（外加 score）。 */
inline void CopyBox(const AlgBox& src, AlgBox* dst) {
    dst->xmin  = src.xmin;
    dst->ymin  = src.ymin;
    dst->xmax  = src.xmax;
    dst->ymax  = src.ymax;
    dst->score = src.score;
}

/* JSON 里 class_names 顺序约定 →  enum 编号对照（详见 alg_types.h）。 */
constexpr int kSpeedLimitKmh[10] = {0, 10, 20, 30, 40, 50, 60, 70, 80, 100};

}  // namespace

int FillAlgResult(const std::vector<Object>& objs, AlgResult* result) {
    AlgFreeResult(result);

    /* 第一遍：按 category attribute 分桶计数。 */
    int tl_n = 0, sl_n = 0;
    for (const auto& o : objs) {
        if (!o.has_box() || !o.has_attributes()) continue;
        const Attribute* cat = FindAttr(o.attributes, "category");
        if (!cat) continue;
        if (cat->value_str == "traffic_light") ++tl_n;
        else if (cat->value_str == "speed_limit") ++sl_n;
        else ALG_LOGW("FillAlgResult: 未知 category '%s'，丢弃", cat->value_str.c_str());
    }

    if (tl_n > 0) {
        result->traffic_lights = static_cast<AlgTrafficLight*>(
            std::calloc(tl_n, sizeof(AlgTrafficLight)));
        if (!result->traffic_lights) return -1;
    }
    if (sl_n > 0) {
        result->speed_limits = static_cast<AlgSpeedLimit*>(
            std::calloc(sl_n, sizeof(AlgSpeedLimit)));
        if (!result->speed_limits) {
            std::free(result->traffic_lights);
            result->traffic_lights = nullptr;
            return -1;
        }
    }

    /* 第二遍：填强类型字段。label 保持 JSON 里 class_names 的下标（0-based），
     * 映射到 enum 时统一 +1（0 留给 INVALID）。 */
    int ti = 0, si = 0;
    for (const auto& o : objs) {
        if (!o.has_box() || !o.has_attributes()) continue;
        const Attribute* cat = FindAttr(o.attributes, "category");
        if (!cat) continue;

        if (cat->value_str == "traffic_light") {
            AlgTrafficLight& dst = result->traffic_lights[ti++];
            CopyBox(o.box, &dst.box);
            if (o.label >= 0 && o.label <= 3) {
                dst.color = static_cast<AlgTrafficLightColor>(o.label + 1);
            } else {
                dst.color = TLC_INVALID;
                ALG_LOGW("traffic_light: label=%d 越界 (期望 0..3)", o.label);
            }
        } else if (cat->value_str == "speed_limit") {
            AlgSpeedLimit& dst = result->speed_limits[si++];
            CopyBox(o.box, &dst.box);
            if (o.label >= 0 && o.label <= 8) {
                dst.value     = static_cast<AlgSpeedLimitValue>(o.label + 1);
                dst.value_kmh = kSpeedLimitKmh[o.label + 1];
            } else {
                dst.value     = SLV_INVALID;
                dst.value_kmh = 0;
                ALG_LOGW("speed_limit: label=%d 越界 (期望 0..8)", o.label);
            }
        }
    }

    result->traffic_light_count = ti;
    result->speed_limit_count   = si;
    return 0;
}

}  // namespace alg
