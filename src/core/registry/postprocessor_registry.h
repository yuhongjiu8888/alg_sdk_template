/**
 * @file postprocessor_registry.h
 * @brief 后处理类型注册表 —— 每个"模型类型"（fcos_face / pfld_landmark / ...）
 * 在自己的源文件里通过 REGISTER_ALG_POST 把构造函数挂进来。
 *
 * 新增模型只要：
 *   1) 派生 IPostprocessor；
 *   2) 在该模型目录里写一行 REGISTER_ALG_POST("my_type", []{ return ...; });
 * 不需要碰 C API、Pipeline、Solution。
 */

#ifndef ALG_CORE_REGISTRY_POSTPROCESSOR_REGISTRY_H
#define ALG_CORE_REGISTRY_POSTPROCESSOR_REGISTRY_H

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/postprocess/postprocessor.h"

namespace alg {

class PostprocessorRegistry {
  public:
    using Builder = std::function<std::unique_ptr<IPostprocessor>()>;

    static PostprocessorRegistry& Instance();

    bool Register(const std::string& type, Builder b);
    std::unique_ptr<IPostprocessor> Create(const std::string& type) const;
    bool Has(const std::string& type) const;
    std::vector<std::string> Types() const;

  private:
    std::unordered_map<std::string, Builder> builders_;
};

namespace detail {
struct PostRegistrar {
    PostRegistrar(const std::string& t, PostprocessorRegistry::Builder b) {
        PostprocessorRegistry::Instance().Register(t, std::move(b));
    }
};
}  // namespace detail

#define ALG_PASTE_INNER(a, b) a##b
#define ALG_PASTE(a, b) ALG_PASTE_INNER(a, b)

#if defined(__GNUC__) || defined(__clang__)
#define ALG_USED __attribute__((used))
#else
#define ALG_USED
#endif

/**
 * 在每个模型 _register.cpp 里调用一次。`__COUNTER__` 保证名字唯一；
 * `used` 防止 release 链接器 --gc-sections 把它当成"没人引用"砍掉。
 */
#define REGISTER_ALG_POST(TYPE, BUILDER)                                                          \
    static ::alg::detail::PostRegistrar ALG_USED ALG_PASTE(_alg_post_reg_, __COUNTER__)(TYPE,     \
                                                                                       BUILDER)

}  // namespace alg

#endif  // ALG_CORE_REGISTRY_POSTPROCESSOR_REGISTRY_H
