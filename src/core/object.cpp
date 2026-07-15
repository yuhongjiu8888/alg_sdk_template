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

    /* 第二遍：填强类型字段。value 由后处理器在 Configure 时从
     * class_names 推导，此处直接读取，不再硬编码映射表。 */
    int ti = 0, si = 0;
    for (const auto& o : objs) {
        if (!o.has_box() || !o.has_attributes()) continue;
        const Attribute* cat = FindAttr(o.attributes, "category");
        if (!cat) continue;

        if (cat->value_str == "traffic_light") {
            AlgTrafficLight& dst = result->traffic_lights[ti++];
            CopyBox(o.box, &dst.box);
            dst.color = (o.value >= TLC_RED && o.value <= TLC_OFF)
                        ? static_cast<AlgTrafficLightColor>(o.value) : TLC_INVALID;
        } else if (cat->value_str == "speed_limit") {
            AlgSpeedLimit& dst = result->speed_limits[si++];
            CopyBox(o.box, &dst.box);
            dst.value = (o.value >= SLV_10 && o.value <= SLV_120)
                        ? static_cast<AlgSpeedLimitValue>(o.value) : SLV_INVALID;
        }
    }

    result->traffic_light_count = ti;
    result->speed_limit_count   = si;
    return 0;
}

}  // namespace alg
