#include "core/object.h"

#include <cstdio>
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

    /* category → AlgSignType（禁令/停车牌）。非禁令类返回 SIGN_INVALID。 */
    auto sign_type_of = [](const std::string& c) -> AlgSignType {
        if (c == "no_parking") return SIGN_NO_PARKING;
        if (c == "pare")       return SIGN_PARE;
        return SIGN_INVALID;
    };

    /* 第一遍：按 category attribute 分桶计数。
     * 注：红绿灯（category=="traffic_light"）当前不对外输出，静默丢弃。 */
    int sl_n = 0, sg_n = 0, lp_n = 0;
    for (const auto& o : objs) {
        if (!o.has_box() || !o.has_attributes()) continue;
        const Attribute* cat = FindAttr(o.attributes, "category");
        if (!cat) continue;
        if (cat->value_str == "traffic_light") continue;   /* 红绿灯：暂不对外输出 */
        else if (cat->value_str == "speed_limit") ++sl_n;
        else if (cat->value_str == "license_plate") ++lp_n;
        else if (sign_type_of(cat->value_str) != SIGN_INVALID) ++sg_n;
        else ALG_LOGW("FillAlgResult: 未知 category '%s'，丢弃", cat->value_str.c_str());
    }

    if (sl_n > 0) {
        result->speed_limits = static_cast<AlgSpeedLimit*>(
            std::calloc(sl_n, sizeof(AlgSpeedLimit)));
        if (!result->speed_limits) {
            AlgFreeResult(result);
            return -1;
        }
    }
    if (sg_n > 0) {
        result->signs = static_cast<AlgSign*>(std::calloc(sg_n, sizeof(AlgSign)));
        if (!result->signs) {
            AlgFreeResult(result);
            return -1;
        }
    }
    if (lp_n > 0) {
        result->license_plates = static_cast<AlgLicensePlate*>(
            std::calloc(lp_n, sizeof(AlgLicensePlate)));
        if (!result->license_plates) {
            AlgFreeResult(result);
            return -1;
        }
    }

    /* 第二遍：填强类型字段。value 由后处理器在 Configure 时从
     * class_names 推导，此处直接读取，不再硬编码映射表。 */
    int si = 0, gi = 0, li = 0;
    for (const auto& o : objs) {
        if (!o.has_box() || !o.has_attributes()) continue;
        const Attribute* cat = FindAttr(o.attributes, "category");
        if (!cat) continue;

        if (cat->value_str == "traffic_light") {
            continue;                            /* 红绿灯：暂不对外输出 */
        } else if (cat->value_str == "speed_limit") {
            AlgSpeedLimit& dst = result->speed_limits[si++];
            CopyBox(o.box, &dst.box);
            dst.value = (o.value >= SLV_10 && o.value <= SLV_120)
                        ? static_cast<AlgSpeedLimitValue>(o.value) : SLV_INVALID;
        } else if (cat->value_str == "license_plate") {
            /* 识别文本 / 单独识别分由 lprnet_rec 后处理器以 attribute 透出。 */
            AlgLicensePlate& dst = result->license_plates[li++];
            CopyBox(o.box, &dst.box);
            const Attribute* text = FindAttr(o.attributes, "text");
            if (text && !text->value_str.empty())
                std::snprintf(dst.text, sizeof(dst.text), "%s", text->value_str.c_str());
            const Attribute* rec = FindAttr(o.attributes, "rec_score");
            dst.rec_score = rec ? rec->value_float : 0.0f;
        } else {
            AlgSignType st = sign_type_of(cat->value_str);
            if (st == SIGN_INVALID) continue;   /* 未知 category：第一遍已告警 */
            AlgSign& dst = result->signs[gi++];
            CopyBox(o.box, &dst.box);
            dst.type = st;                       /* 牌种由 category 直接决定，不依赖 value */
        }
    }

    result->speed_limit_count   = si;
    result->sign_count          = gi;
    result->license_plate_count = li;
    return 0;
}

}  // namespace alg
