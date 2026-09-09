#include "core/solution/chain_solution.h"

#include <algorithm>
#include <map>
#include <unordered_map>

#include "core/logger.h"
#include "core/solution/crop_util.h"

namespace alg {

Status ChainSolution::Init(const SolutionConfig& cfg) {
    /* 1) 构建模型实例集合，并建立 name → 指针映射。 */
    std::unordered_map<std::string, ModelInstance*> by_name;
    model_storage_.clear();
    model_storage_.reserve(cfg.models.size());
    for (const auto& m : cfg.models) {
        auto inst = std::unique_ptr<ModelInstance>(new ModelInstance());
        Status s = inst->Init(m);
        if (s != ALG_OK) {
            ALG_LOGE("ChainSolution: failed to init model '%s' (status=%d)", m.name.c_str(), s);
            return s;
        }
        by_name[m.name] = inst.get();
        model_storage_.push_back(std::move(inst));
    }

    /* 2) 把 stages 里的字符串引用全部解析成索引。 */
    std::unordered_map<std::string, int> stage_idx;
    stages_.clear();
    stages_.reserve(cfg.stages.size());
    for (size_t i = 0; i < cfg.stages.size(); ++i) {
        const StageConfig& sc = cfg.stages[i];
        RuntimeStage rs;
        rs.cfg = sc;
        auto it = by_name.find(sc.model_ref);
        if (it == by_name.end()) {
            ALG_LOGE("stage '%s' references unknown model '%s'",
                     sc.name.c_str(), sc.model_ref.c_str());
            return ALG_E_MODEL_NOT_FOUND;
        }
        rs.model = it->second;
        rs.input_idx = -1;
        rs.output_target = -1;
        if (sc.input_kind == StageInputKind::kObjectsFromStage) {
            auto jt = stage_idx.find(sc.input_stage);
            if (jt == stage_idx.end()) {
                ALG_LOGE("stage '%s' input_stage '%s' not found",
                         sc.name.c_str(), sc.input_stage.c_str());
                return ALG_E_CONFIG;
            }
            rs.input_idx = jt->second;
        }
        if (sc.output_kind != StageOutputKind::kCreateObjects) {
            auto jt = stage_idx.find(sc.output_target);
            if (jt == stage_idx.end()) {
                ALG_LOGE("stage '%s' output_target '%s' not found",
                         sc.name.c_str(), sc.output_target.c_str());
                return ALG_E_CONFIG;
            }
            rs.output_target = jt->second;
        }
        stage_idx[sc.name] = static_cast<int>(i);
        stages_.push_back(std::move(rs));
    }
    stage_produces_.assign(stages_.size(), {});

    /* 3) 提前判定是否需要解码原图（有 ROI 子模型或固定 ROI 裁剪）。 */
    needs_decoded_bgr_ = false;
    for (const auto& rs : stages_) {
        if (rs.cfg.input_kind == StageInputKind::kObjectsFromStage ||
            (rs.cfg.input_kind == StageInputKind::kImage && rs.cfg.roi.enabled)) {
            needs_decoded_bgr_ = true;
            break;
        }
    }
    initialized_ = true;
    ALG_LOGI("ChainSolution: ready, %zu models, %zu stages (decode_bgr=%d)",
             model_storage_.size(), stages_.size(), needs_decoded_bgr_ ? 1 : 0);
    return ALG_OK;
}

namespace {

void MapBackToOriginal(std::vector<Object>* objs, const CropTransform& xf) {
    for (auto& o : *objs) {
        if (o.has_box()) {
            o.box.xmin += xf.offset_x;
            o.box.xmax += xf.offset_x;
            o.box.ymin += xf.offset_y;
            o.box.ymax += xf.offset_y;
        }
    }
}

void MergeFields(Object* dst, Object&& src, StageOutputKind kind) {
    switch (kind) {
        case StageOutputKind::kFillAttributes:
            dst->attributes = std::move(src.attributes);
            dst->field_mask |= ALG_FIELD_ATTRIBUTES;
            break;
        case StageOutputKind::kKeypointsInto:
            dst->keypoints = std::move(src.keypoints);
            dst->field_mask |= ALG_FIELD_KEYPOINTS;
            break;
        case StageOutputKind::kMaskInto:
            dst->mask = std::move(src.mask);
            dst->field_mask |= ALG_FIELD_MASK;
            break;
        case StageOutputKind::kClassifyInto:
            /* 分类器：用 sub 的 label 覆盖 src，把 sub.box.score 当分类置信度乘到
             * src.box.score（detector_score × classifier_conf = 联合置信度）；
             * attributes 全量并过来给 C API 透出。 */
            dst->label     = src.label;
            dst->value     = src.value;
            dst->box.score = dst->box.score * src.box.score;
            if (!src.attributes.empty()) {
                dst->attributes = std::move(src.attributes);
                dst->field_mask |= ALG_FIELD_ATTRIBUTES;
            }
            break;
        case StageOutputKind::kCreateObjects:
            break;
    }
}

/* 给 object 打上 category 属性 + value（透传终端类用）；已有 category 则覆盖。 */
void TagCategory(Object* o, const std::string& category, int value) {
    for (auto& a : o->attributes) {
        if (a.name == "category") { a.value_str = category; o->value = value;
                                    o->field_mask |= ALG_FIELD_ATTRIBUTES; return; }
    }
    Attribute c;
    c.name = "category";
    c.value_str = category;
    o->attributes.push_back(std::move(c));
    o->value = value;
    o->field_mask |= ALG_FIELD_ATTRIBUTES;
}

/* 命中 min_box_short（按 category）→ true 表示该丢弃（框短边 < 阈值）。 */
bool BelowMinBoxShort(const Object& o, const std::map<std::string, int>& m) {
    if (m.empty() || !o.has_attributes()) return false;
    const Attribute* cat = nullptr;
    for (const auto& a : o.attributes) if (a.name == "category") { cat = &a; break; }
    if (!cat) return false;
    auto it = m.find(cat->value_str);
    if (it == m.end()) return false;
    const int w = o.box.xmax - o.box.xmin;
    const int h = o.box.ymax - o.box.ymin;
    return (w < h ? w : h) < it->second;
}

/* 把目标 vector 清空但保留 capacity（每个 Object 内部 vector 也清空保 capacity）。
 * 这样下一帧 push_back / move_assign 不会再 malloc。 */
void ResetStageObjects(std::vector<Object>& v) {
    for (auto& o : v) {
        o.field_mask = 0;
        o.label      = 0;
        o.drop       = false;
        o.attributes.clear();
    }
    v.clear();  /* size=0, capacity 保留 */
}

}  // namespace

Status ChainSolution::RunStage(int stage_idx, const AlgImage& image,
                               const cv::Mat& decoded_bgr) {
    const RuntimeStage& rs = stages_[stage_idx];
    ModelInstance* mi = rs.model;

    auto& out_bucket = stage_produces_[stage_idx].objects;
    /* 注意：本 stage 自己的 bucket 还要复用；只在产出 "objects" 类型时 reset。 */

    /* 按源格式选裁剪路径：NV12/NV21 直接抠原图小区域（省全帧解码）；其余走已解码的
     * BGR Mat（BGR 零拷贝 / RGB/GRAY 在解码时已转 BGR）。 */
    const bool nv_source = (image.format == ALG_PIX_NV12 || image.format == ALG_PIX_NV21);
    auto Crop = [&](const AlgBox& box, const CropConfig& cfg,
                    cv::Mat* holder, AlgImage* out, CropTransform* xf) {
        return nv_source ? CropFromNV12(image, box, cfg, holder, out, xf)
                         : CropFromDecoded(decoded_bgr, box, cfg, holder, out, xf);
    };

    if (rs.cfg.input_kind == StageInputKind::kImage) {
        ResetStageObjects(out_bucket);

        if (rs.cfg.roi.enabled) {
            /* 固定 ROI 裁剪：在原图上裁出指定区域再送模型。 */
            if (!nv_source && decoded_bgr.empty()) {
                ALG_LOGE("ChainSolution: stage '%s' needs roi but decoded_bgr is empty",
                         rs.cfg.name.c_str());
                return ALG_E_PREPROCESS;
            }
            cv::Mat roi_holder;
            AlgImage cropped;
            CropTransform xf;
            AlgBox roi_box;
            roi_box.xmin = rs.cfg.roi.x;
            roi_box.ymin = rs.cfg.roi.y;
            roi_box.xmax = rs.cfg.roi.x + rs.cfg.roi.width;
            roi_box.ymax = rs.cfg.roi.y + rs.cfg.roi.height;
            CropConfig crop_cfg;  /* expand_ratio=1, square=false：不做扩展 */
            if (!Crop(roi_box, crop_cfg, &roi_holder, &cropped, &xf)) {
                ALG_LOGE("ChainSolution: stage '%s' roi crop failed", rs.cfg.name.c_str());
                return ALG_E_PREPROCESS;
            }
            Status r = mi->Run(cropped, &out_bucket);
            if (r != ALG_OK) return r;
            /* 后处理输出的是裁剪图坐标，映射回原图。 */
            MapBackToOriginal(&out_bucket, xf);
            return ALG_OK;
        }

        return mi->Run(image, &out_bucket);
    }

    /* 上游 stage 的 objects 作为 ROI。 */
    std::vector<Object>& src_objs = stage_produces_[rs.input_idx].objects;

    if (rs.cfg.output_kind == StageOutputKind::kCreateObjects) {
        ResetStageObjects(out_bucket);
    }

    cv::Mat roi_holder;
    AlgImage cropped;
    CropTransform xf;
    std::vector<Object> sub;  /* 子模型一次产出（通常 1 个对象），栈对象，小 */

    for (auto& src : src_objs) {
        if (src.drop) continue;
        if (!src.has_box()) continue;

        /* v3.5 终端类透传：上游某 label（如 pare）不进子模型，直接打 category/牌种透出。
         * 命中即跳过裁剪+分类；min_score/min_box_short 作精度兜底。 */
        {
            const PassthroughRule* pt = nullptr;
            for (const auto& r : rs.cfg.passthrough)
                if (r.label == src.label) { pt = &r; break; }
            if (pt) {
                if (pt->min_score > 0.0f && src.box.score < pt->min_score) { src.drop = true; continue; }
                TagCategory(&src, pt->category, pt->sign_value);
                if (BelowMinBoxShort(src, rs.cfg.min_box_short)) src.drop = true;
                continue;
            }
        }

        if (!Crop(src.box, rs.cfg.crop, &roi_holder, &cropped, &xf)) {
            /* 框完全在画外或裁出空 ROI；classify_into 视为分类失败 → drop。 */
            if (rs.cfg.output_kind == StageOutputKind::kClassifyInto) src.drop = true;
            continue;
        }

        sub.clear();
        Status r = mi->Run(cropped, &sub);
        if (r != ALG_OK) return r;
        /* classify_into 用的是 label / box.score 覆盖语义，sub.box 是 ROI 内坐标
         * （分类器一般不输出真实 box），跳过坐标反映射避免污染 src.box。 */
        if (rs.cfg.output_kind != StageOutputKind::kClassifyInto)
            MapBackToOriginal(&sub, xf);

        if (rs.cfg.output_kind == StageOutputKind::kCreateObjects) {
            for (auto& o : sub) out_bucket.push_back(std::move(o));
        } else if (rs.cfg.output_kind == StageOutputKind::kClassifyInto) {
            if (sub.empty()) {
                src.drop = true;
            } else {
                MergeFields(&src, std::move(sub.front()), rs.cfg.output_kind);
                /* 联合分阈值：MergeFields 后 src.box.score = det×cls（最终对外分数）。
                 * 检测/分类各自的 conf_threshold 只卡各自分数，两头都勉强过线时乘积仍可能偏低，
                 * 这里按联合分兜底过滤（0 = 关闭）。这即 deploy_src 的 joint_thr。 */
                if (rs.cfg.score_threshold > 0.0f && src.box.score < rs.cfg.score_threshold)
                    src.drop = true;
                /* v3.5 nopark_min_size：禁停(R-6c)正类仅近处大牌生效（按 category 的最小框短边）。 */
                else if (BelowMinBoxShort(src, rs.cfg.min_box_short))
                    src.drop = true;
            }
        } else {
            if (!sub.empty())
                MergeFields(&src, std::move(sub.front()), rs.cfg.output_kind);
        }
    }
    return ALG_OK;
}

Status ChainSolution::Run(const AlgImage& image, std::vector<Object>* out_objects) {
    return RunInternal(image, out_objects);
}

Status ChainSolution::RunInternal(const AlgImage& image,
                                  std::vector<Object>* out_objects) {
    if (!initialized_) return ALG_E_NOT_INITIALIZED;
    if (!out_objects) return ALG_E_INVALID_ARG;

    /* 整帧只解码一次。把 decoded_bgr_ 作为成员，OpenCV 会自动复用底层 buffer：
     * 同分辨率同格式输入时，cv::cvtColor / 头部构造都不会再分配新内存。
     *
     * 性能关键：NV12/NV21 输入**不**做全帧 YUV→BGR（1080p 一次 ~25ms）——两个主
     * 检测器走 preprocessor 的 NV12 快速路径，子模型的 ROI 裁剪直接走 CropFromNV12
     * 从原图抠小区域转色，都不需要整帧 BGR。其余格式保持原样（BGR 为零拷贝包装）。 */
    const bool nv_source = (image.format == ALG_PIX_NV12 || image.format == ALG_PIX_NV21);
    if (needs_decoded_bgr_ && !nv_source) {
        DecodeSourceToBgrInto(image, &decoded_bgr_);
        if (decoded_bgr_.empty()) {
            ALG_LOGE("ChainSolution: decode source failed");
            return ALG_E_PREPROCESS;
        }
    }

    for (size_t i = 0; i < stages_.size(); ++i) {
        Status r = RunStage(static_cast<int>(i), image, decoded_bgr_);
        if (r != ALG_OK) return r;
    }

    out_objects->clear();
    for (size_t i = 0; i < stages_.size(); ++i) {
        if (stages_[i].cfg.output_kind != StageOutputKind::kCreateObjects) continue;
        for (auto& o : stage_produces_[i].objects) {
            if (o.drop) continue;  /* classify_into 已标记应丢弃的框 */
            out_objects->push_back(std::move(o));
        }
    }
    return ALG_OK;
}

}  // namespace alg
