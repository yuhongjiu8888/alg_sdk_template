#ifndef ALG_CORE_LOG_ALG_LOG_H
#define ALG_CORE_LOG_ALG_LOG_H

/*
 * alg_log —— 借鉴 spdlog 的异步 + 多 sink 设计，给端侧 SDK 增加一个轻量日志模块。
 * ============================================================================
 * 目标（在不破坏现有 ALG_LOGE/W/I/D 接口的前提下）：
 *   1. 异步落盘：调用线程只做 vsnprintf + 拷进环形缓冲，真正的 write/fflush 在
 *      后台线程做。彻底消除"单行太长 → fprintf 系统调用阻塞算法热路径"的问题。
 *   2. 滚动文件：单文件 ≤ 10MB（可配），写满后循环覆盖，保留 N 份。
 *   3. 多 sink：文件 / 控制台 / Android logcat 任意组合。
 *   4. 编译期可配：路径 / 单文件大小 / 保留份数 / 队列深度 / 单行上限，全部 -D 覆盖。
 *
 * 启用方式（默认全关，零热路径开销，行为与改动前完全一致）：
 *   cmake .. -DALG_LOG_FILE=ON \
 *            -DALG_LOG_FILE_PATH=/data/log/alg.log \
 *            -DALG_LOG_FILE_MAX_SIZE=10485760 \
 *            -DALG_LOG_FILE_MAX_FILES=5
 * 配合 -DALG_LOG_INFO=ON / -DALG_LOG_DEBUG=ON 才会把 info/debug 也编进去并落盘。
 *
 * 运行期还可用 alg::log::init(Config{...}) 覆盖路径等（部署时路径常常只有运行期才知道）。
 */

#include <cstddef>
#include <cstdarg>

// ---------------------------------------------------------------------------
// 编译期默认值（用 -D... 覆盖；CMake 选项 ALG_LOG_FILE 会注入这些）
// ---------------------------------------------------------------------------
#ifndef ALG_LOG_FILE_PATH
#  define ALG_LOG_FILE_PATH "alg_sdk.log"        // 文件基路径
#endif
#ifndef ALG_LOG_FILE_MAX_SIZE
#  define ALG_LOG_FILE_MAX_SIZE (10u * 1024u * 1024u)  // 单文件上限：10MB
#endif
#ifndef ALG_LOG_FILE_MAX_FILES
#  define ALG_LOG_FILE_MAX_FILES 5                // 滚动保留份数（循环覆盖）
#endif
#ifndef ALG_LOG_QUEUE_SIZE
#  define ALG_LOG_QUEUE_SIZE 1024                 // 环形缓冲槽位数
#endif
#ifndef ALG_LOG_MSG_MAX
#  define ALG_LOG_MSG_MAX 1024                    // 单行字节上限（超长截断，防 IO 阻塞）
#endif
#ifndef ALG_LOG_ANDROID_DEFAULT
#  if defined(__ANDROID__)
#    define ALG_LOG_ANDROID_DEFAULT true
#  else
#    define ALG_LOG_ANDROID_DEFAULT false
#  endif
#endif

// SDK 以 -fvisibility=hidden 构建；导出这几个入口，便于集成方运行期配置。
#ifndef ALG_LOG_API
#  if defined(_WIN32)
#    define ALG_LOG_API
#  else
#    define ALG_LOG_API __attribute__((visibility("default")))
#  endif
#endif

namespace alg {
namespace log {

// 数值越大越啰嗦（与 level 过滤一致：lvl > Config.level 的消息被丢弃）。
enum class Level : int {
    Error = 0,
    Warn  = 1,
    Info  = 2,
    Debug = 3,
};

struct Config {
    // ---- 滚动文件 sink ----
    bool        to_file       = true;
    const char* file_path     = ALG_LOG_FILE_PATH;       // 基路径，如 /data/log/alg.log
    std::size_t max_file_size = ALG_LOG_FILE_MAX_SIZE;   // 单文件字节上限，写满即滚动
    int         max_files     = ALG_LOG_FILE_MAX_FILES;  // 滚动保留份数（含当前），循环覆盖

    // ---- 控制台 sink ----
    // 沿用旧行为：Error/Warn → stderr，Info/Debug → stdout。
    bool to_console = true;

    // ---- Android logcat sink ----
    // 仅在 __ANDROID__ 下生效；用 __android_log_print 输出，tag 见 android_tag。
    bool        to_android  = ALG_LOG_ANDROID_DEFAULT;
    const char* android_tag = "alg";

    // ---- 异步 worker ----
    bool        async      = true;                 // true：调用方只入队，后台线程落盘
    std::size_t queue_size = ALG_LOG_QUEUE_SIZE;   // 环形槽位数；满了丢弃并计数（不阻塞推理）

    // ---- 运行期 level 过滤 ----
    Level level = Level::Debug;                     // 比该级别更啰嗦的消息直接丢弃
};

// （重新）配置并按需启动后台线程。可重复调用；会先停旧 worker 再应用新配置。
// 不显式调用时，首条日志会用上面的编译期默认值惰性初始化。
ALG_LOG_API void init(const Config& cfg);

// 刷盘并停止后台线程（进程退出前可显式调用；单例析构时也会自动做）。
ALG_LOG_API void shutdown();

// 运行期调整 level 过滤。
ALG_LOG_API void set_level(Level lvl);

// 强制把队列里残留的日志刷到各 sink。
ALG_LOG_API void flush();

// printf 风格入口（ALG_LOG* 宏最终调到这里）。fmt 必须是字面量以便编译器做格式校验。
ALG_LOG_API void writef(Level lvl, const char* fmt, ...)
    __attribute__((format(printf, 2, 3)));

// va_list 版本，供需要转发可变参的场景使用。
ALG_LOG_API void vwritef(Level lvl, const char* fmt, va_list ap);

}  // namespace log
}  // namespace alg

#endif  // ALG_CORE_LOG_ALG_LOG_H
