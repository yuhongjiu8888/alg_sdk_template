#include "core/config/config.h"

#include <fstream>
#include <sstream>
#include <unordered_set>

#include "core/logger.h"

namespace alg {

namespace {

void SetErr(std::string* err, const std::string& msg) {
    ALG_LOGE("%s", msg.c_str());
    if (err) *err = msg;
}

ColorOrder ParseColor(const std::string& s) {
    if (s == "RGB")  return ColorOrder::kRGB;
    if (s == "GRAY" || s == "Gray") return ColorOrder::kGray;
    return ColorOrder::kBGR;
}

ResizeMode ParseResize(const std::string& s) {
    if (s == "stretch")           return ResizeMode::kStretch;
    if (s == "letterbox_center")  return ResizeMode::kLetterboxCenter;
    return ResizeMode::kLetterboxTL; /* 默认与 sgk-sdk 行为一致 */
}

Layout ParseLayout(const std::string& s) {
    if (s == "NHWC") return Layout::kNHWC;
    return Layout::kNCHW;
}

bool ParsePreprocess(const Json::Value& v, PreprocessConfig* cfg, std::string* err) {
    if (!v.isObject()) { SetErr(err, "preprocess must be an object"); return false; }

    if (v.isMember("input_size")) {
        const Json::Value& sz = v["input_size"];
        if (!sz.isArray() || sz.size() != 2) {
            SetErr(err, "preprocess.input_size must be [w, h]");
            return false;
        }
        cfg->net_width  = sz[0].asInt();
        cfg->net_height = sz[1].asInt();
    } else {
        cfg->net_width  = v.get("input_width",  0).asInt();
        cfg->net_height = v.get("input_height", 0).asInt();
    }
    if (cfg->net_width <= 0 || cfg->net_height <= 0) {
        SetErr(err, "preprocess.input_size missing or non-positive");
        return false;
    }

    cfg->color  = ParseColor(v.get("color",  "BGR").asString());
    cfg->resize = ParseResize(v.get("resize", "letterbox_tl").asString());
    cfg->layout = ParseLayout(v.get("layout", "NCHW").asString());

    if (v.isMember("mean") && v["mean"].isArray()) {
        for (Json::ArrayIndex i = 0; i < v["mean"].size() && i < 3; ++i)
            cfg->mean[i] = v["mean"][i].asFloat();
    }
    if (v.isMember("std") && v["std"].isArray()) {
        for (Json::ArrayIndex i = 0; i < v["std"].size() && i < 3; ++i)
            cfg->std[i] = v["std"][i].asFloat();
    }
    cfg->scale     = v.get("scale", 1.0f).asFloat();
    cfg->pad_value = static_cast<uint8_t>(v.get("pad_value", 0).asInt());
    return true;
}

bool ParseModelInstance(const std::string& name, const Json::Value& v,
                        ModelInstanceConfig* m, std::string* err) {
    if (!v.isObject()) {
        SetErr(err, "models." + name + " must be an object");
        return false;
    }
    m->name = name;
    m->model_path = v.get("model_path", "").asString();
    if (m->model_path.empty()) {
        SetErr(err, "models." + name + ".model_path is required");
        return false;
    }
    if (!v.isMember("preprocess") || !ParsePreprocess(v["preprocess"], &m->pre, err))
        return false;
    if (!v.isMember("postprocess") || !v["postprocess"].isObject()) {
        SetErr(err, "models." + name + ".postprocess is required");
        return false;
    }
    m->post_type   = v["postprocess"].get("type", "").asString();
    if (m->post_type.empty()) {
        SetErr(err, "models." + name + ".postprocess.type is required");
        return false;
    }
    m->post_params = v["postprocess"];
    return true;
}

bool ParseStage(const Json::Value& v, StageConfig* s, std::string* err) {
    s->name = v.get("name", "").asString();
    if (s->name.empty()) { SetErr(err, "stage.name is required"); return false; }
    s->model_ref = v.get("model", "").asString();
    if (s->model_ref.empty()) { SetErr(err, "stage.model is required"); return false; }

    std::string in = v.get("input", "image").asString();
    if (in == "image") {
        s->input_kind = StageInputKind::kImage;
    } else {
        /* "objects_from:<stage_name>" */
        const std::string kPrefix = "objects_from:";
        if (in.rfind(kPrefix, 0) != 0) {
            SetErr(err, "stage.input must be 'image' or 'objects_from:<stage>'");
            return false;
        }
        s->input_kind = StageInputKind::kObjectsFromStage;
        s->input_stage = in.substr(kPrefix.size());
    }
    if (v.isMember("crop") && v["crop"].isObject()) {
        s->crop.expand_ratio = v["crop"].get("expand_ratio", 1.0f).asFloat();
        s->crop.square       = v["crop"].get("square", false).asBool();
    }

    std::string produces = v.get("produces", "objects").asString();
    if (produces == "objects") {
        s->output_kind = StageOutputKind::kCreateObjects;
    } else {
        /* "keypoints_into:<stage>" / "attributes_into:<stage>" / "embedding_into:<stage>" */
        auto parse_into = [&](const std::string& tag, StageOutputKind kind) -> int {
            const std::string prefix = tag + ":";
            if (produces.rfind(prefix, 0) != 0) return 0;
            s->output_kind = kind;
            s->output_target = produces.substr(prefix.size());
            return 1;
        };
        if (!parse_into("keypoints_into",  StageOutputKind::kFillKeypoints) &&
            !parse_into("attributes_into", StageOutputKind::kFillAttributes) &&
            !parse_into("embedding_into",  StageOutputKind::kFillEmbedding) &&
            !parse_into("classify_into",   StageOutputKind::kClassifyInto)) {
            SetErr(err, "stage.produces unrecognized: " + produces);
            return false;
        }
    }
    return true;
}

}  // namespace

AlgStatus LoadSolutionConfig(const std::string& path, SolutionConfig* out, std::string* err_msg) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) { SetErr(err_msg, "cannot open config: " + path); return ALG_E_IO; }

    Json::CharReaderBuilder builder;
    Json::Value root;
    std::string parse_err;
    if (!Json::parseFromStream(builder, ifs, &root, &parse_err)) {
        SetErr(err_msg, "JSON parse error: " + parse_err);
        return ALG_E_CONFIG;
    }

    /* solution */
    if (!root.isMember("solution") || !root["solution"].isObject()) {
        SetErr(err_msg, "config.solution missing"); return ALG_E_CONFIG;
    }
    out->solution_type = root["solution"].get("type", "chain").asString();

    /* models */
    if (!root.isMember("models") || !root["models"].isObject()) {
        SetErr(err_msg, "config.models missing"); return ALG_E_CONFIG;
    }
    const Json::Value& models = root["models"];
    out->models.clear();
    for (const auto& name : models.getMemberNames()) {
        ModelInstanceConfig m;
        if (!ParseModelInstance(name, models[name], &m, err_msg)) return ALG_E_CONFIG;
        out->models.push_back(std::move(m));
    }
    if (out->models.empty()) {
        SetErr(err_msg, "config.models is empty"); return ALG_E_CONFIG;
    }

    /* stages */
    const Json::Value& stages = root["solution"]["stages"];
    if (!stages.isArray() || stages.empty()) {
        SetErr(err_msg, "config.solution.stages must be a non-empty array");
        return ALG_E_CONFIG;
    }
    std::unordered_set<std::string> known_models;
    for (const auto& m : out->models) known_models.insert(m.name);

    out->stages.clear();
    std::unordered_set<std::string> seen_stage_names;
    for (Json::ArrayIndex i = 0; i < stages.size(); ++i) {
        StageConfig s;
        if (!ParseStage(stages[i], &s, err_msg)) return ALG_E_CONFIG;
        if (!known_models.count(s.model_ref)) {
            SetErr(err_msg, "stage '" + s.name + "' references unknown model '" + s.model_ref + "'");
            return ALG_E_CONFIG;
        }
        if (s.input_kind == StageInputKind::kObjectsFromStage &&
            !seen_stage_names.count(s.input_stage)) {
            SetErr(err_msg, "stage '" + s.name + "' inputs from unknown earlier stage '" +
                                s.input_stage + "'");
            return ALG_E_CONFIG;
        }
        if (s.output_kind != StageOutputKind::kCreateObjects &&
            !seen_stage_names.count(s.output_target)) {
            SetErr(err_msg, "stage '" + s.name + "' outputs into unknown earlier stage '" +
                                s.output_target + "'");
            return ALG_E_CONFIG;
        }
        seen_stage_names.insert(s.name);
        out->stages.push_back(std::move(s));
    }
    return ALG_OK;
}

}  // namespace alg
