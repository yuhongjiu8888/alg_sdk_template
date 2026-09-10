#ifndef ALG_CORE_LOGGER_H
#define ALG_CORE_LOGGER_H

#include <cstdio>

#include "core/log/alg_log.h"

/*
 * 日志策略
 * --------
 * - 默认等级 Warn，只输出 Error/Warn。
 * - 所有等级均编入，通过 AlgSetLogLevel 在运行期动态过滤。
 * - ALG_LOG_INFO / ALG_LOG_DEBUG 构建选项仅改变启动时默认等级。
 *
 * fprintf 走系统调用 + locale + 锁，单次开销 ~10–50µs。每帧多次就吃掉了
 * 端侧推理的"算法时间预算"中相当大一块，因此宏会先做一次原子等级判断；被
 * 过滤的日志不会格式化参数、加锁或执行 I/O。
 *
 * 落盘 / 异步 / Android（可选）
 * ----------------------------
 * 加 -DALG_LOG_FILE=ON（CMake 选项，见根 CMakeLists.txt）后，上面四个宏会改走
 * core/log/alg_log 的**异步多 sink** 实现：调用线程只 vsnprintf + 入队，真正的
 * write/fflush 在后台线程做——既解决"单行太长 fprintf 阻塞热路径"，又支持
 * 滚动文件（单文件 ≤10MB 可配、循环覆盖）和 Android logcat。
 * 不加该开关时，行为与原来逐字节一致（直接 fprintf，零额外开销）。
 *
 * 调用方接口（ALG_LOGE/W/I/D 的 printf 风格用法）保持完全不变。
 */

#define ALG_LOG_IS_ENABLED(lvl) (::alg::log::is_enabled((lvl)))

#if defined(ALG_ENABLE_LOG_FILE)
#define ALG_LOG__EMIT(lvl, fmt, ...)                                      \
    do {                                                                  \
        if (ALG_LOG_IS_ENABLED(lvl))                                      \
            ::alg::log::writef((lvl), fmt, ##__VA_ARGS__);                \
    } while (0)
#else
#define ALG_LOG__EMIT_TO(stream, lvl, fmt, ...)                           \
    do {                                                                  \
        if (ALG_LOG_IS_ENABLED(lvl))                                      \
            std::fprintf((stream), fmt "\n", ##__VA_ARGS__);             \
    } while (0)
#endif

#if defined(ALG_ENABLE_LOG_FILE)
#define ALG_LOGE(fmt, ...) ALG_LOG__EMIT(::alg::log::Level::Error, "[alg][E] " fmt, ##__VA_ARGS__)
#define ALG_LOGW(fmt, ...) ALG_LOG__EMIT(::alg::log::Level::Warn,  "[alg][W] " fmt, ##__VA_ARGS__)
#define ALG_LOGI(fmt, ...) ALG_LOG__EMIT(::alg::log::Level::Info,  "[alg][I] " fmt, ##__VA_ARGS__)
#define ALG_LOGD(fmt, ...) ALG_LOG__EMIT(::alg::log::Level::Debug, "[alg][D] " fmt, ##__VA_ARGS__)
#else
#define ALG_LOGE(fmt, ...) ALG_LOG__EMIT_TO(stderr, ::alg::log::Level::Error, "[alg][E] " fmt, ##__VA_ARGS__)
#define ALG_LOGW(fmt, ...) ALG_LOG__EMIT_TO(stderr, ::alg::log::Level::Warn,  "[alg][W] " fmt, ##__VA_ARGS__)
#define ALG_LOGI(fmt, ...) ALG_LOG__EMIT_TO(stdout, ::alg::log::Level::Info,  "[alg][I] " fmt, ##__VA_ARGS__)
#define ALG_LOGD(fmt, ...) ALG_LOG__EMIT_TO(stderr, ::alg::log::Level::Debug, "[alg][D] " fmt, ##__VA_ARGS__)
#endif

#endif  // ALG_CORE_LOGGER_H
