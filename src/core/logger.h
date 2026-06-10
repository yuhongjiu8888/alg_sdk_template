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
 */

#define ALG_LOGE(fmt, ...) std::fprintf(stderr, "[alg][E] " fmt "\n", ##__VA_ARGS__)
#define ALG_LOGW(fmt, ...) std::fprintf(stderr, "[alg][W] " fmt "\n", ##__VA_ARGS__)

#ifdef ALG_ENABLE_LOG_INFO
#define ALG_LOGI(fmt, ...) std::fprintf(stdout, "[alg][I] " fmt "\n", ##__VA_ARGS__)
#define ALG_PROFILE 1
#else
#define ALG_LOGI(fmt, ...) ((void)0)
#endif

#ifdef ALG_ENABLE_LOG_DEBUG
#define ALG_LOGD(fmt, ...) std::fprintf(stderr, "[alg][D] " fmt "\n", ##__VA_ARGS__)
#else
#define ALG_LOGD(fmt, ...) ((void)0)
#endif

#endif  // ALG_CORE_LOGGER_H
