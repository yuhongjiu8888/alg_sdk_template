#include "core/object.h"

#include <cstdio>
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

void FillAlgResult(const std::vector<Object>& objs, AlgResult* result) {
    result->speed_limit_count = 0;
    result->sign_count = 0;
    result->license_plate_count = 0;

    /* category → AlgSignType（禁令/停车牌）。非禁令类返回 SIGN_INVALID。 */
    auto sign_type_of = [](const std::string& c) -> AlgSignType {
        if (c == "no_parking") return SIGN_NO_PARKING;
        if (c == "pare")       return SIGN_PARE;
        return SIGN_INVALID;
    };

    /* 单遍填充固定容量数组。value 由后处理器在 Configure 时从
     * class_names 推导，此处直接读取，不再硬编码映射表。 */
    for (const auto& o : objs) {
        if (!o.has_box() || !o.has_attributes()) continue;
        const Attribute* cat = FindAttr(o.attributes, "category");
        if (!cat) continue;

        if (cat->value_str == "traffic_light") {
            continue;                            /* 红绿灯：暂不对外输出 */
        } else if (cat->value_str == "speed_limit") {
            if (result->speed_limit_count >= ALG_MAX_SPEED_LIMIT_RESULTS) {
                ALG_LOGW("FillAlgResult: 限速牌结果超过容量 %d，截断",
                         ALG_MAX_SPEED_LIMIT_RESULTS);
                continue;
            }
            AlgSpeedLimit& dst = result->speed_limits[result->speed_limit_count++];
            dst = AlgSpeedLimit{};
            CopyBox(o.box, &dst.box);
            dst.value = (o.value >= SLV_10 && o.value <= SLV_120)
                        ? static_cast<AlgSpeedLimitValue>(o.value) : SLV_INVALID;
        } else if (cat->value_str == "license_plate") {
            /* 识别文本 / 单独识别分由 lprnet_rec 后处理器以 attribute 透出。 */
            if (result->license_plate_count >= ALG_MAX_LICENSE_PLATE_RESULTS) {
                ALG_LOGW("FillAlgResult: 车牌结果超过容量 %d，截断",
                         ALG_MAX_LICENSE_PLATE_RESULTS);
                continue;
            }
            AlgLicensePlate& dst =
                result->license_plates[result->license_plate_count++];
            dst = AlgLicensePlate{};
            CopyBox(o.box, &dst.box);
            const Attribute* text = FindAttr(o.attributes, "text");
            if (text && !text->value_str.empty())
                std::snprintf(dst.text, sizeof(dst.text), "%s", text->value_str.c_str());
            const Attribute* rec = FindAttr(o.attributes, "rec_score");
            dst.rec_score = rec ? rec->value_float : 0.0f;
        } else {
            AlgSignType st = sign_type_of(cat->value_str);
            if (st == SIGN_INVALID) {
                ALG_LOGW("FillAlgResult: 未知 category '%s'，丢弃",
                         cat->value_str.c_str());
                continue;
            }
            if (result->sign_count >= ALG_MAX_SIGN_RESULTS) {
                ALG_LOGW("FillAlgResult: 禁令牌结果超过容量 %d，截断",
                         ALG_MAX_SIGN_RESULTS);
                continue;
            }
            AlgSign& dst = result->signs[result->sign_count++];
            dst = AlgSign{};
            CopyBox(o.box, &dst.box);
            dst.type = st;                       /* 牌种由 category 直接决定，不依赖 value */
        }
    }

}

}  // namespace alg
