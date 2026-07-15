#ifndef ALG_CORE_LOGGER_H
#define ALG_CORE_LOGGER_H

#include <cstdio>

/*
 * 日志策略
 * --------
 * - ALG_LOGE / ALG_LOGW：永远开（错误/真告警必备）。
 * - ALG_LOGI：每帧耗时等 info，默认 **编译期消除**；加 -DALG_ENABLE_LOG_INFO 开启。
 * - ALG_LOGD：板端调试诊断 dump（xmm/in/out/det/cls-diag 这类逐帧详情），
 *   默认 **编译期消除**；需要排查时在编译命令里加 -DALG_ENABLE_LOG_DEBUG 开启。
 *
 * fprintf 走系统调用 + locale + 锁，单次开销 ~10–50µs。每帧多次就吃掉了
 * 端侧推理的"算法时间预算"中相当大一块——所以 info/debug 默认不编进去。
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

#if defined(ALG_ENABLE_LOG_FILE)
#include "core/log/alg_log.h"
#define ALG_LOG__EMIT(lvl, fmt, ...) ::alg::log::writef((lvl), fmt, ##__VA_ARGS__)
#endif

#if defined(ALG_ENABLE_LOG_FILE)
#define ALG_LOGE(fmt, ...) ALG_LOG__EMIT(::alg::log::Level::Error, "[alg][E] " fmt, ##__VA_ARGS__)
#define ALG_LOGW(fmt, ...) ALG_LOG__EMIT(::alg::log::Level::Warn,  "[alg][W] " fmt, ##__VA_ARGS__)
#else
#define ALG_LOGE(fmt, ...) std::fprintf(stderr, "[alg][E] " fmt "\n", ##__VA_ARGS__)
#define ALG_LOGW(fmt, ...) std::fprintf(stderr, "[alg][W] " fmt "\n", ##__VA_ARGS__)
#endif

#ifdef ALG_ENABLE_LOG_INFO
#if defined(ALG_ENABLE_LOG_FILE)
#define ALG_LOGI(fmt, ...) ALG_LOG__EMIT(::alg::log::Level::Info, "[alg][I] " fmt, ##__VA_ARGS__)
#else
#define ALG_LOGI(fmt, ...) std::fprintf(stdout, "[alg][I] " fmt "\n", ##__VA_ARGS__)
#endif
#define ALG_PROFILE 1
#else
#define ALG_LOGI(fmt, ...) ((void)0)
#endif

#ifdef ALG_ENABLE_LOG_DEBUG
#if defined(ALG_ENABLE_LOG_FILE)
#define ALG_LOGD(fmt, ...) ALG_LOG__EMIT(::alg::log::Level::Debug, "[alg][D] " fmt, ##__VA_ARGS__)
#else
#define ALG_LOGD(fmt, ...) std::fprintf(stderr, "[alg][D] " fmt "\n", ##__VA_ARGS__)
#endif
#else
#define ALG_LOGD(fmt, ...) ((void)0)
#endif

#endif  // ALG_CORE_LOGGER_H
