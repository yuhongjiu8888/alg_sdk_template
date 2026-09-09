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
    if (s == "letterbox_tl_fit")  return ResizeMode::kLetterboxTLFit;
    return ResizeMode::kLetterboxTL; /* 默认与 sgk-sdk 行为一致 */
}

Layout ParseLayout(const std::string& s) {
    if (s == "NHWC") return Layout::kNHWC;
    return Layout::kNCHW;
}

PreprocessEngine ParseEngine(const std::string& s) {
    if (s == "opencv") return PreprocessEngine::kOpenCV;
    if (s == "aipp") return PreprocessEngine::kAipp;
    return PreprocessEngine::kAuto;
}

InputFormatPolicy ParseInputFormat(const std::string& s) {
    if (s == "NV12" || s == "nv12") return InputFormatPolicy::kNV12;
    if (s == "NV21" || s == "nv21") return InputFormatPolicy::kNV21;
    return InputFormatPolicy::kAuto;
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
    cfg->scale = v.get("scale", 1.0f).asFloat();
    const int pad_value = v.get("pad_value", 0).asInt();
    if (pad_value < 0 || pad_value > 255) {
        SetErr(err, "preprocess.pad_value must be in [0, 255]");
        return false;
    }
    cfg->pad_value = static_cast<uint8_t>(pad_value);
    if (cfg->std[0] == 0.0f || cfg->std[1] == 0.0f || cfg->std[2] == 0.0f) {
        SetErr(err, "preprocess.std values must be non-zero");
        return false;
    }
    const std::string engine = v.get("engine", "auto").asString();
    if (engine != "auto" && engine != "opencv" && engine != "aipp") {
        SetErr(err, "preprocess.engine must be auto, opencv or aipp");
        return false;
    }
    cfg->engine = ParseEngine(engine);
    const std::string input_format = v.get("input_format", "auto").asString();
    if (input_format != "auto" && input_format != "NV12" && input_format != "nv12" &&
        input_format != "NV21" && input_format != "nv21") {
        SetErr(err, "preprocess.input_format must be auto, NV12 or NV21");
        return false;
    }
    cfg->input_format = ParseInputFormat(input_format);
    if (v.isMember("max_input_size")) {
        const Json::Value& max_sz = v["max_input_size"];
        if (!max_sz.isArray() || max_sz.size() != 2 ||
            max_sz[0].asInt() <= 0 || max_sz[1].asInt() <= 0) {
            SetErr(err, "preprocess.max_input_size must be [w, h]");
            return false;
        }
        cfg->max_input_width = max_sz[0].asInt();
        cfg->max_input_height = max_sz[1].asInt();
        if (cfg->max_input_width > 4096 || cfg->max_input_height > 4096) {
            SetErr(err, "preprocess.max_input_size exceeds CV610 AIPP limit 4096");
            return false;
        }
    }
    if (v.isMember("aipp_output_size")) {
        const Json::Value& aipp_sz = v["aipp_output_size"];
        if (!aipp_sz.isArray() || aipp_sz.size() != 2 ||
            aipp_sz[0].asInt() <= 0 || aipp_sz[1].asInt() <= 0) {
            SetErr(err, "preprocess.aipp_output_size must be [w, h]");
            return false;
        }
        cfg->aipp_output_width = aipp_sz[0].asInt();
        cfg->aipp_output_height = aipp_sz[1].asInt();
    }
    if (v.isMember("aipp_graph_padding")) {
        const Json::Value& pad = v["aipp_graph_padding"];
        if (!pad.isArray() || pad.size() != 4) {
            SetErr(err, "preprocess.aipp_graph_padding must be [left, top, right, bottom]");
            return false;
        }
        cfg->graph_pad_left = pad[0].asInt();
        cfg->graph_pad_top = pad[1].asInt();
        cfg->graph_pad_right = pad[2].asInt();
        cfg->graph_pad_bottom = pad[3].asInt();
        if (cfg->graph_pad_left < 0 || cfg->graph_pad_top < 0 ||
            cfg->graph_pad_right < 0 || cfg->graph_pad_bottom < 0) {
            SetErr(err, "preprocess.aipp_graph_padding values must be non-negative");
            return false;
        }
    }
    const bool custom_aipp_output = cfg->aipp_output_width > 0;
    const bool graph_padding = cfg->graph_pad_left || cfg->graph_pad_top ||
                               cfg->graph_pad_right || cfg->graph_pad_bottom;
    if (custom_aipp_output != graph_padding ||
        (custom_aipp_output &&
         (cfg->aipp_output_width + cfg->graph_pad_left + cfg->graph_pad_right !=
              cfg->net_width ||
          cfg->aipp_output_height + cfg->graph_pad_top + cfg->graph_pad_bottom !=
              cfg->net_height))) {
        SetErr(err, "aipp_output_size plus aipp_graph_padding must equal model input_size");
        return false;
    }
    if (custom_aipp_output && cfg->resize == ResizeMode::kStretch) {
        SetErr(err, "aipp_graph_padding cannot be used with stretch resize");
        return false;
    }
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
        s->crop.pad_value    = v["crop"].get("pad_value", -1).asInt();
    }
    if (v.isMember("roi") && v["roi"].isObject()) {
        const Json::Value& r = v["roi"];
        s->roi.enabled = true;
        s->roi.x      = r.get("x", 0).asInt();
        s->roi.y      = r.get("y", 0).asInt();
        s->roi.width  = r.get("width", 0).asInt();
        s->roi.height = r.get("height", 0).asInt();
    }
    s->score_threshold = v.get("score_threshold", 0.0f).asFloat();

    /* v3.5 终端类透传：passthrough:[{label,category,sign_value,min_score}]。 */
    if (v.isMember("passthrough") && v["passthrough"].isArray()) {
        for (Json::ArrayIndex i = 0; i < v["passthrough"].size(); ++i) {
            const Json::Value& p = v["passthrough"][i];
            PassthroughRule r;
            r.label      = p.get("label", -1).asInt();
            r.category   = p.get("category", "").asString();
            r.sign_value = p.get("sign_value", 0).asInt();
            r.min_score  = p.get("min_score", 0.0f).asFloat();
            if (r.label < 0 || r.category.empty()) {
                SetErr(err, "stage.passthrough entry needs label>=0 and non-empty category");
                return false;
            }
            s->passthrough.push_back(std::move(r));
        }
    }

    /* v3.5 按 category 的最小框短边过滤（nopark_min_size）：min_box_short:{"no_parking":40}。 */
    if (v.isMember("min_box_short") && v["min_box_short"].isObject()) {
        const Json::Value& m = v["min_box_short"];
        for (const auto& key : m.getMemberNames())
            s->min_box_short[key] = m[key].asInt();
    }

    std::string produces = v.get("produces", "objects").asString();
    if (produces == "objects") {
        s->output_kind = StageOutputKind::kCreateObjects;
    } else {
        /* "attributes_into:<stage>" / "classify_into:<stage>" */
        auto parse_into = [&](const std::string& tag, StageOutputKind kind) -> int {
            const std::string prefix = tag + ":";
            if (produces.rfind(prefix, 0) != 0) return 0;
            s->output_kind = kind;
            s->output_target = produces.substr(prefix.size());
            return 1;
        };
        if (!parse_into("attributes_into", StageOutputKind::kFillAttributes) &&
            !parse_into("classify_into",   StageOutputKind::kClassifyInto) &&
            !parse_into("keypoints_into",  StageOutputKind::kKeypointsInto) &&
            !parse_into("mask_into",       StageOutputKind::kMaskInto)) {
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
        if (s.roi.enabled && s.input_kind != StageInputKind::kImage) {
            SetErr(err_msg, "stage '" + s.name + "': roi is only valid for input='image'");
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
