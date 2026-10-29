#pragma once

// 统一日志门面：C++ / Android(Java 经 JNI) / iOS(ObjC、Swift) 共用一套级别、tag 与 sink，
// 平台差异收敛在本文件。

#ifndef SPDLOG_ACTIVE_LEVEL
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/sinks/dist_sink.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#if defined(__ANDROID__)
// 回调线程上绝不能让 android_sink 在 EAGAIN 时 sleep 重试
#ifndef SPDLOG_ANDROID_RETRIES
#define SPDLOG_ANDROID_RETRIES 0
#endif
#include <spdlog/sinks/android_sink.h>
#elif defined(__APPLE__)
#include <os/log.h>
#endif

namespace beat::log {

struct Config {
    std::string logDir;                            // 空 = 不写文件
    spdlog::level::level_enum level = spdlog::level::trace;
    std::size_t fileBytes = std::size_t(2) << 20;  // 2 MiB
    std::size_t fileCount = 3;
    bool fileEnabled = true;                       // 只控制写文件
};

#if defined(__APPLE__)

/** os_log sink：category 取 logger 名，保留 Console.app 按 tag 过滤。 */
class OsLogSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit OsLogSink(const std::string& tag)
        : log_(os_log_create("com.runbeat.pro", tag.c_str())) {}

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        // os_log 自带时间戳与 category，与 android_sink 的 use_raw_msg 保持一致
        os_log_with_type(log_, ToOsType(msg.level), "%{public}.*s",
                         static_cast<int>(msg.payload.size()), msg.payload.data());
    }

    void flush_() override {}

private:
    static os_log_type_t ToOsType(spdlog::level::level_enum level) noexcept {
        switch (level) {
            case spdlog::level::trace:
            case spdlog::level::debug: return OS_LOG_TYPE_DEBUG;
            // info 也用 DEFAULT：os_log 的 info 级默认只进内存缓冲，`log show` 事后查不到
            case spdlog::level::info:
            case spdlog::level::warn: return OS_LOG_TYPE_DEFAULT;
            case spdlog::level::err: return OS_LOG_TYPE_ERROR;
            default: return OS_LOG_TYPE_FAULT;
        }
    }

    os_log_t log_;
};

#endif

namespace detail {

inline constexpr std::string_view kTickTag = "RunBeat-Beat";
inline constexpr const char* kPattern = "[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v";

/** 门面的进程级状态：级别、每拍 logger 与落盘目标。 */
struct State {
    std::atomic<bool> inited{false};
    std::atomic<spdlog::level::level_enum> level{spdlog::level::info};

    /** 文件 sink 的挂载点：摘掉子 sink 即彻底停止落盘。 */
    std::shared_ptr<spdlog::sinks::dist_sink_mt> gate =
            std::make_shared<spdlog::sinks::dist_sink_mt>();

    std::shared_ptr<spdlog::logger> tick;      // 音频回调使用
    std::shared_ptr<spdlog::logger> fallback;  // 注册失败兜底

    // 晚绑定：关闭文件日志时不创建文件
    std::shared_ptr<spdlog::sinks::sink> file;
    std::string dir;
    std::size_t bytes = 0;
    std::size_t count = 0;
    bool attached = false;
};

inline State& state() noexcept {
    static State instance;
    return instance;
}

inline std::vector<spdlog::sink_ptr> MakeSinks(const std::string& tag, bool realtime) {
    std::vector<spdlog::sink_ptr> sinks;
#if defined(__ANDROID__)
    if (realtime) {
        sinks.push_back(std::make_shared<spdlog::sinks::android_sink_st>(tag, true));
    } else {
        sinks.push_back(std::make_shared<spdlog::sinks::android_sink_mt>(tag, true));
    }
#elif defined(__APPLE__)
    (void)realtime;
    sinks.push_back(std::make_shared<OsLogSink>(tag));
#else
    (void)tag;
    (void)realtime;
    sinks.push_back(std::make_shared<spdlog::sinks::null_sink_mt>());
#endif
    if (!realtime) sinks.push_back(state().gate);
    return sinks;
}

inline std::shared_ptr<spdlog::logger> Fallback() noexcept {
    auto& ref = state().fallback;
    if (!ref) {
        try {
            ref = std::make_shared<spdlog::logger>("null", std::make_shared<spdlog::sinks::null_sink_mt>());
        } catch (...) {
        }
    }
    return ref;
}

} // namespace detail

inline std::shared_ptr<spdlog::logger> MakeLogger(const std::string& tag, bool realtime) {
    if (auto existing = spdlog::get(tag)) return existing;

    auto sinks = detail::MakeSinks(tag, realtime);
    auto logger = std::make_shared<spdlog::logger>(tag, sinks.begin(), sinks.end());
    // register_logger 不应用全局级别，需显式继承，否则懒创建的 logger 停在 info
    logger->set_level(detail::state().level.load(std::memory_order_relaxed));
    spdlog::register_logger(logger);
    return logger;
}

/** 取 logger；调用方无需判空。 */
inline std::shared_ptr<spdlog::logger> Get(std::string_view tag) noexcept {
    try {
        if (auto logger = spdlog::get(std::string(tag))) return logger;
        return MakeLogger(std::string(tag), false);
    } catch (...) {
        // 并发首次注册同一 tag 时后注册者会抛；再取一次，仍没有才回落 null sink
        try {
            if (auto logger = spdlog::get(std::string(tag))) return logger;
        } catch (...) {
        }
        return detail::Fallback();
    }
}

/** 音频回调线程专用：避开 registry 锁与引用计数。 */
inline spdlog::logger* TickLogger() noexcept {
    return detail::state().tick.get();
}

/** UI 开关：只停落盘，系统日志照旧。仅初始化与 UI 线程调用（串行）。 */
inline void SetFileEnabled(bool enabled) noexcept {
    auto& s = detail::state();
    try {
        if (!enabled) {
            if (s.attached && s.file) {
                s.file->flush();  // 摘挂前 flush，否则要等下次挂载才写出
                s.gate->remove_sink(s.file);
            }
            s.attached = false;
            return;
        }
        if (s.dir.empty()) return;
        if (!s.file) {
            s.file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                    s.dir + "/runbeat.log", s.bytes, s.count);
            // dist_sink 只转发原始消息，格式必须直接下发给文件 sink
            s.file->set_pattern(detail::kPattern);
        }
        if (!s.attached) {
            s.gate->add_sink(s.file);
            s.attached = true;
        }
    } catch (...) {
        // 目录不可写或磁盘满：降级为只打系统日志
    }
}

inline void Init(const Config& config) noexcept {
    auto& s = detail::state();
    if (s.inited.exchange(true)) return;

    s.level.store(config.level, std::memory_order_relaxed);
    try {
        spdlog::set_level(config.level);
    } catch (...) {
    }

    try {
        s.dir = config.logDir;
        s.bytes = config.fileBytes;
        s.count = config.fileCount;
    } catch (...) {
    }
    SetFileEnabled(config.fileEnabled);

    try {
        s.tick = MakeLogger(std::string(detail::kTickTag), true);
        if (s.tick) {
            // 默认 handler 会在回调线程取静态锁、调 localtime 再 fprintf，这里直接丢弃
            s.tick->set_error_handler([](const std::string&) {});
        }
    } catch (...) {
    }

    try {
        spdlog::flush_every(std::chrono::seconds(3));
    } catch (...) {
    }
}

inline void Shutdown() noexcept {
    auto& s = detail::state();
    if (!s.inited.exchange(false)) return;
    try {
        spdlog::apply_all([](std::shared_ptr<spdlog::logger> logger) { logger->flush(); });
        SetFileEnabled(false);
        spdlog::shutdown();
    } catch (...) {
    }
    s.tick.reset();
    s.file.reset();
}

} // namespace beat::log
