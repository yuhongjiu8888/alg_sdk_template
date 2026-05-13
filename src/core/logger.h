#ifndef ALG_CORE_LOGGER_H
#define ALG_CORE_LOGGER_H

#include <cstdio>

/*
 * 日志策略
 * --------
 * - ALG_LOGE / ALG_LOGW：永远开（错误/警告诊断必备）
 * - ALG_LOGI：默认 **编译期消除**，避免每帧 fprintf 拖累热路径。
 *   开发期需要看每帧耗时时，在编译命令里加 -DALG_ENABLE_LOG_INFO 即可。
 *
 * fprintf 走系统调用 + locale + 锁，单次开销 ~10–50µs。每帧多次就吃掉了
 * 端侧推理的"算法时间预算"中相当大一块。
 */

#define ALG_LOGE(fmt, ...) std::fprintf(stderr, "[alg][E] " fmt "\n", ##__VA_ARGS__)
#define ALG_LOGW(fmt, ...) std::fprintf(stderr, "[alg][W] " fmt "\n", ##__VA_ARGS__)

#ifdef ALG_ENABLE_LOG_INFO
#define ALG_LOGI(fmt, ...) std::fprintf(stdout, "[alg][I] " fmt "\n", ##__VA_ARGS__)
#define ALG_PROFILE 1
#else
#define ALG_LOGI(fmt, ...) ((void)0)
#endif

#endif  // ALG_CORE_LOGGER_H
