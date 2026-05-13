#include "core/object.h"

#include <cstdlib>
#include <cstring>

#include "alg_interface.h"

namespace alg {

namespace {

AlgAttributes* NewAttributes(const std::vector<Attribute>& attrs) {
    auto* out = static_cast<AlgAttributes*>(std::calloc(1, sizeof(AlgAttributes)));
    if (!out) return nullptr;
    out->count = static_cast<int>(attrs.size());
    if (out->count == 0) return out;
    out->items = static_cast<AlgAttribute*>(std::calloc(out->count, sizeof(AlgAttribute)));
    if (!out->items) { std::free(out); return nullptr; }
    for (int i = 0; i < out->count; ++i) {
        const Attribute& a = attrs[i];
        std::strncpy(out->items[i].name, a.name.c_str(), sizeof(out->items[i].name) - 1);
        out->items[i].value_int = a.value_int;
        out->items[i].value_float = a.value_float;
        std::strncpy(out->items[i].value_str, a.value_str.c_str(),
                     sizeof(out->items[i].value_str) - 1);
    }
    return out;
}

}  // namespace

int FillAlgResult(const std::vector<Object>& objs, AlgResult* result) {
    AlgFreeResult(result);
    result->object_count = static_cast<int>(objs.size());
    if (objs.empty()) {
        result->objects = nullptr;
        return 0;
    }
    result->objects =
        static_cast<AlgObject*>(std::calloc(objs.size(), sizeof(AlgObject)));
    if (!result->objects) { result->object_count = 0; return -1; }

    for (size_t i = 0; i < objs.size(); ++i) {
        const Object& src = objs[i];
        AlgObject& dst = result->objects[i];
        dst.field_mask = src.field_mask;
        dst.box = src.box;
        if (src.has_attributes()) dst.attributes = NewAttributes(src.attributes);
    }
    return 0;
}

}  // namespace alg
