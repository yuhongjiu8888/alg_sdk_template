#include "core/infer/aipp_scale.h"
#include <atomic>

namespace alg {
namespace aipp_scale {
namespace {

/* 进程级全局开关，与 alg_log.cpp 的 g_level 同构：无锁原子，可在运行期改写。 */
std::atomic<int> g_mode{static_cast<int>(Mode::kAuto)};

}  // namespace

Mode mode() {
    return static_cast<Mode>(g_mode.load(std::memory_order_relaxed));
}

void set_mode(Mode mode) {
    g_mode.store(static_cast<int>(mode), std::memory_order_relaxed);
}

}  // namespace aipp_scale
}  // namespace alg
