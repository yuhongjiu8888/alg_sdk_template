#include "core/registry/postprocessor_registry.h"

namespace alg {

PostprocessorRegistry& PostprocessorRegistry::Instance() {
    static PostprocessorRegistry inst;
    return inst;
}

bool PostprocessorRegistry::Register(const std::string& type, Builder b) {
    if (builders_.count(type)) return false;
    builders_.emplace(type, std::move(b));
    return true;
}

std::unique_ptr<IPostprocessor> PostprocessorRegistry::Create(const std::string& type) const {
    auto it = builders_.find(type);
    if (it == builders_.end()) return nullptr;
    return it->second();
}

bool PostprocessorRegistry::Has(const std::string& type) const {
    return builders_.count(type) > 0;
}

std::vector<std::string> PostprocessorRegistry::Types() const {
    std::vector<std::string> out;
    out.reserve(builders_.size());
    for (auto& kv : builders_) out.push_back(kv.first);
    return out;
}

}  // namespace alg
