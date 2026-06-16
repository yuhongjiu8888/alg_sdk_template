#include "core/log/alg_log.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>

#include <sys/stat.h>   // mkdir / stat
#include <sys/types.h>
#include <unistd.h>     // getpid

#if defined(__ANDROID__)
#  include <android/log.h>
#endif

namespace alg {
namespace log {
namespace {

// ---------------------------------------------------------------------------
// 一条待落盘的日志（环形缓冲的槽位）。定长，避免热路径上的堆分配。
// ---------------------------------------------------------------------------
struct Slot {
    struct timespec ts;                 // 产生时刻（调用线程采集，保证时间/顺序准确）
    Level           level = Level::Info;
    int             len   = 0;          // body 实际长度
    char            body[ALG_LOG_MSG_MAX];
};

// 组装好"时间前缀 + 正文 + 换行"后，一行最多这么长。
constexpr int kPrefixMax = 28;          // "2026-06-16 12:00:00.123 "
constexpr int kLineMax   = kPrefixMax + ALG_LOG_MSG_MAX + 2;

int format_prefix(char* out, const struct timespec& ts) {
    struct tm tmv;
    time_t sec = ts.tv_sec;
    localtime_r(&sec, &tmv);
    int ms = static_cast<int>(ts.tv_nsec / 1000000);
    return std::snprintf(out, kPrefixMax + 1,
                         "%04d-%02d-%02d %02d:%02d:%02d.%03d ",
                         tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                         tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms);
}

// 尽力 mkdir -p 出文件所在目录（失败不致命，fopen 会再报错）。
void ensure_parent_dir(const std::string& path) {
    std::string::size_type pos = 0;
    while ((pos = path.find('/', pos + 1)) != std::string::npos) {
        std::string dir = path.substr(0, pos);
        if (!dir.empty()) ::mkdir(dir.c_str(), 0755);
    }
}

#if defined(__ANDROID__)
int to_android_prio(Level lvl) {
    switch (lvl) {
        case Level::Error: return ANDROID_LOG_ERROR;
        case Level::Warn:  return ANDROID_LOG_WARN;
        case Level::Info:  return ANDROID_LOG_INFO;
        case Level::Debug: return ANDROID_LOG_DEBUG;
    }
    return ANDROID_LOG_INFO;
}
#endif

// ---------------------------------------------------------------------------
// 滚动文件 sink：单文件写满 max_size 即滚动，循环保留 max_files 份。
//   base, base.1, base.2, ... base.N   （base.N 为最旧，滚动时被覆盖）
// 只在 worker 线程里被调用，因此自身不加锁。
// ---------------------------------------------------------------------------
class RotatingFileSink {
public:
    void configure(const std::string& base, std::size_t max_size, int max_files) {
        close();
        base_      = base;
        max_size_  = max_size;
        max_files_ = max_files;
        open_(/*truncate=*/false);   // 续写已有文件，直到写满再滚动
    }

    void write(const char* data, std::size_t n) {
        if (!fp_) return;
        // 单行就超过整文件上限：直接写（容忍一个偏大的文件），否则会死循环滚动。
        if (max_size_ > 0 && cur_size_ > 0 && cur_size_ + n > max_size_) rotate();
        cur_size_ += std::fwrite(data, 1, n, fp_);
    }

    void flush() { if (fp_) std::fflush(fp_); }

    void close() {
        if (fp_) { std::fflush(fp_); std::fclose(fp_); fp_ = nullptr; }
        cur_size_ = 0;
    }

private:
    bool open_(bool truncate) {
        ensure_parent_dir(base_);
        fp_ = std::fopen(base_.c_str(), truncate ? "wb" : "ab");
        if (!fp_) return false;
        std::fseek(fp_, 0, SEEK_END);
        long sz = std::ftell(fp_);
        cur_size_ = (sz > 0) ? static_cast<std::size_t>(sz) : 0;
        return true;
    }

    void rotate() {
        if (fp_) { std::fclose(fp_); fp_ = nullptr; }
        if (max_files_ > 1) {
            std::string oldest = base_ + "." + std::to_string(max_files_ - 1);
            std::remove(oldest.c_str());
            for (int i = max_files_ - 2; i >= 1; --i) {
                std::string src = base_ + "." + std::to_string(i);
                std::string dst = base_ + "." + std::to_string(i + 1);
                std::rename(src.c_str(), dst.c_str());
            }
            std::rename(base_.c_str(), (base_ + ".1").c_str());
        }
        open_(/*truncate=*/true);     // 全新当前文件
    }

    std::string base_;
    std::FILE*  fp_       = nullptr;
    std::size_t cur_size_ = 0;
    std::size_t max_size_ = 0;
    int         max_files_ = 1;
};

// ---------------------------------------------------------------------------
// Logger 单例：配置 + 环形队列 + worker 线程 + 各 sink 扇出。
// ---------------------------------------------------------------------------
class Logger {
public:
    static Logger& instance() {
        static Logger inst;     // C++11 起函数局部静态初始化线程安全
        return inst;
    }

    void init(const Config& cfg) {
        std::lock_guard<std::mutex> lk(cfg_mtx_);
        apply_config_locked(cfg);
        initialized_.store(true, std::memory_order_release);
    }

    void set_level(Level lvl) { level_.store(static_cast<int>(lvl), std::memory_order_relaxed); }

    void submit(Level lvl, const struct timespec& ts, const char* msg, int len) {
        ensure_init();
        if (static_cast<int>(lvl) > level_.load(std::memory_order_relaxed)) return;

        if (!async_) {                          // 同步：直接落盘（调试用）
            Slot s;
            fill_slot(s, lvl, ts, msg, len);
            std::lock_guard<std::mutex> lk(sink_mtx_);
            write_line(s);
            flush_sinks();
            return;
        }

        std::unique_lock<std::mutex> lk(q_mtx_);
        if (count_ == ring_.size()) {           // 队列满：丢弃 + 计数，绝不阻塞推理
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // 唤醒合并：worker 仅在队列为空时才会 wait（谓词 count_>0||stop_），
        // 因此只有 0→1 这次入队需要 notify。worker 正在连续消费(count_>0)时
        // 再入队，它回到循环即可看到，无需唤醒——省掉高频日志下的 futex 唤醒/
        // 上下文切换。q_mtx_ 串行化"自增"与"判谓词/进 wait"，不会丢唤醒。
        const bool was_empty = (count_ == 0);
        fill_slot(ring_[tail_], lvl, ts, msg, len);
        tail_ = (tail_ + 1) % ring_.size();
        ++count_;
        lk.unlock();
        if (was_empty) q_cv_.notify_one();
    }

    void flush() {
        if (!async_) return;
        // 等队列清空后再刷一次。
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(q_mtx_);
                if (count_ == 0) break;
            }
            std::this_thread::yield();
        }
        std::lock_guard<std::mutex> lk(sink_mtx_);
        flush_sinks();
    }

    void shutdown() {
        std::lock_guard<std::mutex> lk(cfg_mtx_);
        stop_worker_locked();
        std::lock_guard<std::mutex> sk(sink_mtx_);
        file_.close();
    }

    ~Logger() {
        stop_worker_locked();
        file_.close();
    }

private:
    Logger() : level_(static_cast<int>(Level::Debug)) {}

    void ensure_init() {
        if (initialized_.load(std::memory_order_acquire)) return;
        std::lock_guard<std::mutex> lk(cfg_mtx_);
        if (initialized_.load(std::memory_order_relaxed)) return;
        apply_config_locked(Config{});          // 编译期默认值惰性初始化
        initialized_.store(true, std::memory_order_release);
    }

    // 持有 cfg_mtx_ 时调用。
    void apply_config_locked(const Config& cfg) {
        stop_worker_locked();

        level_.store(static_cast<int>(cfg.level), std::memory_order_relaxed);
        console_     = cfg.to_console;
        file_enabled_ = cfg.to_file;
        async_       = cfg.async;
#if defined(__ANDROID__)
        android_     = cfg.to_android;
        android_tag_ = cfg.android_tag ? cfg.android_tag : "alg";
#endif
        {
            std::lock_guard<std::mutex> sk(sink_mtx_);
            file_.close();
            if (file_enabled_ && cfg.file_path && cfg.file_path[0]) {
                file_.configure(cfg.file_path, cfg.max_file_size,
                                cfg.max_files > 0 ? cfg.max_files : 1);
            } else {
                file_enabled_ = false;
            }
        }

        if (async_) {
            std::size_t cap = cfg.queue_size > 0 ? cfg.queue_size : 1;
            {
                std::lock_guard<std::mutex> lk(q_mtx_);
                ring_.assign(cap, Slot{});
                head_ = tail_ = count_ = 0;
            }
            start_worker_locked();   // 横幅由 worker 启动时打（见 worker_main）
        } else {
            write_banner();          // 同步模式无 worker，这里直接打一次
        }
    }

    // 会话起始分割线（仿 glog）：每次 worker 启动 / init 只打一次，文件+终端都打，
    // 是日志流里的第一行——重启后一眼能看出新一段从哪开始。绕过 level 过滤，恒打。
    void write_banner() {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        char body[ALG_LOG_MSG_MAX];
        int n = std::snprintf(
            body, sizeof(body),
            "================ alg_sdk log start | pid=%ld ================",
            static_cast<long>(getpid()));
        if (n < 0) return;
        Slot s;
        fill_slot(s, Level::Info, ts, body, n);
        std::lock_guard<std::mutex> sk(sink_mtx_);
        write_line(s);
        flush_sinks();   // 立即可见，不等队列攒满
    }

    void start_worker_locked() {
        stop_.store(false, std::memory_order_relaxed);
        worker_ = std::thread([this] { worker_main(); });
    }

    void stop_worker_locked() {
        if (!worker_.joinable()) return;
        {
            std::lock_guard<std::mutex> lk(q_mtx_);
            stop_.store(true, std::memory_order_relaxed);
        }
        q_cv_.notify_all();
        worker_.join();
    }

    void fill_slot(Slot& s, Level lvl, const struct timespec& ts, const char* msg, int len) {
        s.ts    = ts;
        s.level = lvl;
        if (len < 0) len = 0;
        if (len > ALG_LOG_MSG_MAX - 1) len = ALG_LOG_MSG_MAX - 1;  // 截断超长行
        std::memcpy(s.body, msg, len);
        s.body[len] = '\0';
        s.len = len;
    }

    void worker_main() {
        write_banner();   // 保证横幅是本会话日志流的第一行（先于任何已入队的消息）
        for (;;) {
            Slot s;
            bool drained;
            {
                std::unique_lock<std::mutex> lk(q_mtx_);
                q_cv_.wait(lk, [this] { return count_ > 0 || stop_.load(std::memory_order_relaxed); });
                if (count_ == 0) break;          // stop 且已清空
                s = ring_[head_];                // 必须在持锁期间拷出（槽位随后会被复用）
                head_ = (head_ + 1) % ring_.size();
                --count_;
                drained = (count_ == 0);
            }
            {
                std::lock_guard<std::mutex> sk(sink_mtx_);
                write_line(s);
                if (drained) {
                    report_dropped_locked();
                    flush_sinks();
                }
            }
        }
        std::lock_guard<std::mutex> sk(sink_mtx_);
        report_dropped_locked();
        flush_sinks();
    }

    // 持有 sink_mtx_ 时调用：把一条日志扇出到各 sink。
    void write_line(const Slot& s) {
        char line[kLineMax];
        int p = format_prefix(line, s.ts);
        std::memcpy(line + p, s.body, s.len);
        p += s.len;
        line[p++] = '\n';

        if (file_enabled_) file_.write(line, static_cast<std::size_t>(p));
        if (console_) {
            std::FILE* st = (s.level <= Level::Warn) ? stderr : stdout;
            std::fwrite(line, 1, static_cast<std::size_t>(p), st);
        }
#if defined(__ANDROID__)
        if (android_) __android_log_print(to_android_prio(s.level), android_tag_, "%s", s.body);
#endif
    }

    // 持有 sink_mtx_ 时调用。
    void report_dropped_locked() {
        uint64_t now = dropped_.load(std::memory_order_relaxed);
        if (now == dropped_reported_) return;
        char msg[96];
        int n = std::snprintf(msg, sizeof(msg),
                              "[alg][W] log queue overflow: %llu message(s) dropped",
                              static_cast<unsigned long long>(now - dropped_reported_));
        dropped_reported_ = now;
        Slot s;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        fill_slot(s, Level::Warn, ts, msg, n);
        write_line(s);
    }

    void flush_sinks() {
        if (file_enabled_) file_.flush();
        if (console_) { std::fflush(stdout); std::fflush(stderr); }
    }

    // ---- 配置 / 生命周期 ----
    std::mutex        cfg_mtx_;
    std::atomic<bool> initialized_{false};
    std::atomic<int>  level_;
    bool              console_      = true;
    bool              file_enabled_ = false;
    bool              async_        = true;
#if defined(__ANDROID__)
    bool              android_      = false;
    const char*       android_tag_  = "alg";
#endif

    // ---- 环形队列 ----
    std::mutex              q_mtx_;
    std::condition_variable q_cv_;
    std::vector<Slot>       ring_;
    std::size_t             head_ = 0, tail_ = 0, count_ = 0;
    std::atomic<bool>       stop_{false};
    std::atomic<uint64_t>   dropped_{0};
    uint64_t                dropped_reported_ = 0;
    std::thread             worker_;

    // ---- sink ----
    std::mutex        sink_mtx_;
    RotatingFileSink  file_;
};

}  // namespace

// ---------------------------------------------------------------------------
// 公开 API
// ---------------------------------------------------------------------------
void init(const Config& cfg)   { Logger::instance().init(cfg); }
void shutdown()                { Logger::instance().shutdown(); }
void set_level(Level lvl)      { Logger::instance().set_level(lvl); }
void flush()                   { Logger::instance().flush(); }

void vwritef(Level lvl, const char* fmt, va_list ap) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);   // 事件时刻在调用线程采集
    char buf[ALG_LOG_MSG_MAX];
    int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0) return;
    if (n > ALG_LOG_MSG_MAX - 1) n = ALG_LOG_MSG_MAX - 1;   // vsnprintf 返回的是"本应写入"的长度
    Logger::instance().submit(lvl, ts, buf, n);
}

void writef(Level lvl, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vwritef(lvl, fmt, ap);
    va_end(ap);
}

}  // namespace log
}  // namespace alg
