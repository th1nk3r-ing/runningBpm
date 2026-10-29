#pragma once

// 统一日志门面：C++ / Android(Java 经 JNI) / iOS(ObjC、Swift) 共用一套级别、tag 与 sink，
// 平台差异收敛在本文件。

#ifndef SPDLOG_ACTIVE_LEVEL
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
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
    std::size_t fileBytes = std::size_t(4) << 20;  // 4 MiB
    std::size_t fileCount = 2;
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

/** 回调线程 → 落盘线程的单生产者环形队列，满即丢，不分配不加锁。 */
inline constexpr std::size_t kBeatSlots = 256;
inline constexpr std::size_t kBeatTextMax = 96;
static_assert((kBeatSlots & (kBeatSlots - 1)) == 0, "kBeatSlots 必须是 2 的幂");
inline constexpr auto kBeatDrainInterval = std::chrono::milliseconds(100);

struct BeatRecord {
    spdlog::log_clock::time_point time{};
    spdlog::level::level_enum level = spdlog::level::trace;
    std::uint16_t size = 0;
    char text[kBeatTextMax]{};
};

class BeatQueue final {
public:
    bool Push(const char* data, std::size_t size, spdlog::level::level_enum level,
              spdlog::log_clock::time_point time) noexcept {
        const std::uint32_t head = head_.load(std::memory_order_relaxed);
        if (head - tail_.load(std::memory_order_acquire) >= kBeatSlots) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        BeatRecord& slot = slots_[head & (kBeatSlots - 1)];
        slot.size = static_cast<std::uint16_t>(size < kBeatTextMax ? size : kBeatTextMax);
        std::memcpy(slot.text, data, slot.size);
        slot.level = level;
        slot.time = time;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    bool Pop(BeatRecord& out) noexcept {
        const std::uint32_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        out = slots_[tail & (kBeatSlots - 1)];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    std::uint32_t TakeDropped() noexcept {
        return dropped_.exchange(0, std::memory_order_relaxed);
    }

    /** 重新 Init 前丢弃上一次的残留记录。 */
    void Reset() noexcept {
        tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
        dropped_.store(0, std::memory_order_relaxed);
    }

private:
    std::array<BeatRecord, kBeatSlots> slots_{};
    std::atomic<std::uint32_t> head_{0};
    std::atomic<std::uint32_t> tail_{0};
    std::atomic<std::uint32_t> dropped_{0};
};

inline BeatQueue& beatQueue() noexcept {
    static BeatQueue queue;
    return queue;
}

class BeatQueueSink final : public spdlog::sinks::base_sink<spdlog::details::null_mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        beatQueue().Push(msg.payload.data(), msg.payload.size(), msg.level, msg.time);
    }
    void flush_() override {}
};

struct State {
    std::atomic<bool> inited{false};
    std::atomic<spdlog::level::level_enum> level{spdlog::level::info};

    /** 文件 sink 的挂载点：摘掉子 sink 即彻底停止落盘。 */
    std::shared_ptr<spdlog::sinks::dist_sink_mt> gate =
            std::make_shared<spdlog::sinks::dist_sink_mt>();

    std::shared_ptr<spdlog::logger> tick;
    std::shared_ptr<spdlog::logger> fallback;  // 注册失败兜底
    /** tick logger 的系统日志 sink：drain 线程报丢弃数时用。 */
    std::shared_ptr<spdlog::sinks::sink> tickSystemSink;

    std::thread drain;
    std::atomic<bool> draining{false};
    std::mutex lifecycle;

    // 晚绑定：关闭文件日志时不创建文件
    std::shared_ptr<spdlog::sinks::sink> file;
    std::string dir;
    std::size_t bytes = 0;
    std::size_t count = 0;
    bool attached = false;

    /** 漏调 Shutdown 时也不能带着 joinable 线程退出。 */
    ~State() {
        draining.store(false, std::memory_order_relaxed);
        if (drain.joinable()) drain.join();
    }
};

inline State& state() noexcept {
    static State instance;
    return instance;
}

/** 排空队列写进 gate，文件开关因而同样生效。 */
inline void DrainBeats() noexcept {
    auto& s = state();
    BeatRecord record;
    while (beatQueue().Pop(record)) {
        spdlog::details::log_msg msg(record.time, spdlog::source_loc{},
                                     spdlog::string_view_t(kTickTag.data(), kTickTag.size()),
                                     record.level,
                                     spdlog::string_view_t(record.text, record.size));
        try {
            s.gate->log(msg);
        } catch (...) {
        }
    }
    const std::uint32_t dropped = beatQueue().TakeDropped();
    if (dropped > 0) {
        char text[48];
        const int len = std::snprintf(text, sizeof(text), "beat log dropped=%u", dropped);
        if (len > 0) {
            const auto size = static_cast<std::size_t>(len) < sizeof(text)
                                      ? static_cast<std::size_t>(len)
                                      : sizeof(text) - 1;
            spdlog::details::log_msg msg(spdlog::string_view_t(kTickTag.data(), kTickTag.size()),
                                         spdlog::level::warn,
                                         spdlog::string_view_t(text, size));
            try {
                // 系统日志同样要有：文件开关关闭时缺口只能在这里看到
                if (s.tickSystemSink) s.tickSystemSink->log(msg);
                s.gate->log(msg);
            } catch (...) {
            }
        }
    }
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
    // Apple 没有无锁的系统日志 sink：realtime 只入队，避免给回调挂 std::mutex
    if (realtime) {
        sinks.push_back(std::make_shared<spdlog::sinks::null_sink_st>());
    } else {
        sinks.push_back(std::make_shared<OsLogSink>(tag));
    }
#else
    (void)tag;
    (void)realtime;
    sinks.push_back(std::make_shared<spdlog::sinks::null_sink_mt>());
#endif
    if (realtime) sinks.push_back(std::make_shared<BeatQueueSink>());
    else sinks.push_back(state().gate);
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
        // 未初始化时不许建文件：否则会在 Shutdown 后按旧 dir 重开
        if (!s.inited.load(std::memory_order_relaxed)) return;
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
    try {
        // Init/Shutdown 串行化：否则会在 joinable 线程上重新赋值
        std::lock_guard<std::mutex> lock(s.lifecycle);
        if (s.inited.exchange(true)) return;

        s.level.store(config.level, std::memory_order_relaxed);
        try {
            spdlog::set_level(config.level);
        } catch (...) {
        }

        try {
            if (s.dir != config.logDir || s.bytes != config.fileBytes || s.count != config.fileCount) {
                // 落盘目标变了：旧 sink 不能复用
                if (s.attached && s.file) s.gate->remove_sink(s.file);
                s.file.reset();
                s.attached = false;
            }
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
                const auto& sinks = s.tick->sinks();
                s.tickSystemSink = sinks.empty() ? nullptr : sinks.front();
            }
        } catch (...) {
        }

        try {
            spdlog::flush_every(std::chrono::seconds(3));
        } catch (...) {
        }

        try {
            detail::beatQueue().Reset();
            s.draining.store(true, std::memory_order_relaxed);
            s.drain = std::thread([] {
                auto& st = detail::state();
                while (st.draining.load(std::memory_order_relaxed)) {
                    detail::DrainBeats();
                    std::this_thread::sleep_for(detail::kBeatDrainInterval);
                }
                detail::DrainBeats();
            });
        } catch (...) {
            s.draining.store(false, std::memory_order_relaxed);
        }
    } catch (...) {
    }
}

inline void Shutdown() noexcept {
    auto& s = detail::state();
    try {
        std::lock_guard<std::mutex> lock(s.lifecycle);
        if (!s.inited.exchange(false)) return;
        s.draining.store(false, std::memory_order_relaxed);
        if (s.drain.joinable()) s.drain.join();
        detail::DrainBeats();
        spdlog::apply_all([](std::shared_ptr<spdlog::logger> logger) { logger->flush(); });
        SetFileEnabled(false);
        spdlog::shutdown();
        s.tickSystemSink.reset();
        s.file.reset();
        // tick 故意不 reset：音频线程可能仍持有裸指针，等下次 Init 覆写
    } catch (...) {
    }
}

} // namespace beat::log
