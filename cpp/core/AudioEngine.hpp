#pragma once

#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <thread>

#include "Clock.hpp"
#include "Constants.hpp"
#include "SamplePlayer.hpp"
#include "Limiter.hpp"

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "Audio callback requires lock-free 64-bit atomics");
static_assert(std::atomic<double>::is_always_lock_free,
              "Audio callback requires lock-free double atomics");

inline void WriteSilence(float* out, int numFrames) noexcept {
    for (int i = 0; i < numFrames; ++i) out[i] = 0.0f;
}

/**
 * RAII 守卫：把 OnAudioCallback 注册为"活跃 callback"，供 BeginSampleUpdate 等待。
 * 入口两次 seq_cst 检查 loading_，关闭"更新在首次检查与发布计数之间开始"的竞争窗口。
 */
class RealtimeSection {
public:
    RealtimeSection() = default;
    // 裸 atomic 指针 + 析构 fetch_sub：拷贝会双重减计数使 activeCallbacks_ 下溢、永久自旋
    RealtimeSection(const RealtimeSection&) = delete;
    RealtimeSection& operator=(const RealtimeSection&) = delete;
    RealtimeSection(RealtimeSection&&) = delete;
    RealtimeSection& operator=(RealtimeSection&&) = delete;

    [[nodiscard]] bool TryEnter(std::atomic<bool>& loading,
                                std::atomic<unsigned>& active) noexcept {
        loading_ = &loading;
        active_ = &active;
        if (loading_->load(std::memory_order_seq_cst)) {  // seq_cst 确保看到最新值
            return false;
        }
        active_->fetch_add(1, std::memory_order_seq_cst);
        if (loading_->load(std::memory_order_seq_cst)) {
            active_->fetch_sub(1, std::memory_order_seq_cst);
            return false;
        }
        entered_ = true;
        return true;
    }

    ~RealtimeSection() noexcept {
        if (entered_) active_->fetch_sub(1, std::memory_order_seq_cst);
    }

private:
    std::atomic<bool>* loading_ = nullptr;
    std::atomic<unsigned>* active_ = nullptr;
    bool entered_ = false;
};

/**
 * 音频引擎状态机 — 整合 Clock、SamplePlayer、Limiter。
 * 音频回调为单一串行线程，禁止锁/分配/日志；控制线程参数经 std::atomic 交换、
 * 在 callback 入口一次性加载；Clock 仅 callback 线程访问；样本更新等待在途 callback。
 */
class AudioEngine {
public:
    enum class State : int { Idle, Running, Paused };
    enum class AccentMode : int {
        AllHeavy = 0, AllLight = 1, Every2 = 2, Every2Light = 3, Every4 = 4, Every4Light = 5
    };

    AudioEngine() = default;
    ~AudioEngine() { Stop(); }

    void Start(double bpm) noexcept {
        bpm_.store(beat::SanitizeBpm(bpm), std::memory_order_relaxed);
        beatSeq_.store(0, std::memory_order_relaxed);
        framesRendered_.store(0, std::memory_order_relaxed);
        // generation 合并请求：callback 应用最新 BPM 并重置相位
        startGeneration_.fetch_add(1, std::memory_order_release);
        state_.store(State::Running, std::memory_order_release);
    }

    void Stop() noexcept {
        state_.store(State::Idle, std::memory_order_release);
        pendingChimeTriggers_.store(0, std::memory_order_relaxed);
        tickPlayer_.Stop();
        tickLoPlayer_.Stop();
        chimePlayer_.Stop();
    }

    void Pause() noexcept {
        state_.store(State::Paused, std::memory_order_release);
        pendingChimeTriggers_.store(0, std::memory_order_relaxed);
    }

    void Resume() noexcept {
        if (state_.load(std::memory_order_acquire) == State::Paused) {
            state_.store(State::Running, std::memory_order_release);
        }
    }

    void SetBpm(double bpm) noexcept {
        // NaN/Inf/0/负数会让 Clock 进入"参数已变但节奏不变"的不可自愈状态。
        bpm_.store(beat::SanitizeBpm(bpm), std::memory_order_relaxed);
    }

    void SetTickVolume(double vol) noexcept {
        tickGain_.store(vol, std::memory_order_relaxed);
    }

    void SetChimeVolume(double vol) noexcept {
        chimeGain_.store(vol, std::memory_order_relaxed);
    }

    void SetAccentMode(int mode) noexcept {
        if (mode < static_cast<int>(AccentMode::AllHeavy) ||
            mode > static_cast<int>(AccentMode::Every4Light)) {
            mode = static_cast<int>(AccentMode::AllHeavy);
        }
        accentMode_.store(mode, std::memory_order_relaxed);
    }

    /** 输出总增益（x1/x2/x3）：混音时乘入 tick/chime，末级由 Limiter 限幅兜底。 */
    void SetOutputGain(double gain) noexcept {
        outputGain_.store(gain, std::memory_order_relaxed);
    }

    /** 请求更新采样率，由下一次 callback 应用；非法值（含 0）丢弃。 */
    void SetSampleRate(int sr) noexcept {
        if (!beat::IsValidSampleRate(sr)) return;
        requestedSampleRate_.store(sr, std::memory_order_relaxed);
    }

    void TriggerChime() noexcept {
        if (state_.load(std::memory_order_relaxed) != State::Running) return;
        // Voice 由 callback 线程独占，控制线程只排队有界请求
        unsigned pending = pendingChimeTriggers_.load(std::memory_order_relaxed);
        while (pending < SamplePlayer::kVoices &&
               !pendingChimeTriggers_.compare_exchange_weak(
                       pending, pending + 1,
                       std::memory_order_release,
                       std::memory_order_relaxed)) {
        }
    }

    [[nodiscard]] State GetState() const noexcept {
        return state_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t GetBeatSeq() const noexcept {
        return beatSeq_.load(std::memory_order_relaxed);
    }

    /** 每拍回调（音频线程内同步调用，beatSeq 从 1 起）。注册信息用 seqlock 发布：
     *  奇数→写字段→偶数，读侧校验前后序号一致，避免指针成对性被撕裂；写侧无互斥，调用方需保证单线程。 */
    using TickCallback = void (*)(void* userData, std::uint64_t beatSeq,
                                  std::uint64_t framePos, int sampleRate);
    void SetTickCallback(TickCallback cb, void* userData) noexcept {
        tickCallbackSeq_.fetch_add(1, std::memory_order_acq_rel);   // → 奇数：写入中
        tickCallback_.store(cb, std::memory_order_relaxed);
        tickCallbackUserData_.store(userData, std::memory_order_relaxed);
        tickCallbackSeq_.fetch_add(1, std::memory_order_release);   // → 偶数：已发布
    }

    void LoadTickSamples(const float* data, size_t length) noexcept {
        SampleUpdateScope scope(*this);
        if (!scope.Entered()) return;   // 音频线程内调用：拒绝，避免自旋死锁
        tickPlayer_.Load(data, length);
    }

    void LoadTickLoSamples(const float* data, size_t length) noexcept {
        SampleUpdateScope scope(*this);
        if (!scope.Entered()) return;
        tickLoPlayer_.Load(data, length);
    }

    void LoadChimeSamples(const float* data, size_t length) noexcept {
        SampleUpdateScope scope(*this);
        if (!scope.Entered()) return;
        chimePlayer_.Load(data, length);
    }

    /** 运行时替换 tick 样本：更新期间 callback 输出静音；音频线程内调用会被拒绝。 */
    void LoadSoundPack(const float* tickHi, size_t tickHiLen,
                        const float* tickLo, size_t tickLoLen) noexcept {
        SampleUpdateScope scope(*this);
        if (!scope.Entered()) return;
        tickPlayer_.Load(tickHi, tickHiLen);
        tickLoPlayer_.Load(tickLo, tickLoLen);
    }

    void OnAudioCallback(float* out, int numFrames) noexcept {
        // 标记当前线程在音频回调内：Load* 据此拒绝更新（否则会等自己退出 → 死锁）
        struct AudioThreadScope {
            AudioThreadScope() noexcept { inAudioCallback_ = true; }
            ~AudioThreadScope() noexcept { inAudioCallback_ = false; }
        } audioThreadScope;
#ifndef NDEBUG
        if (callbackActive_.test_and_set(std::memory_order_acquire)) {
            assert(false && "AudioEngine::OnAudioCallback must not be reentrant");
            return;
        }
        struct CallbackReentryGuard {
            std::atomic_flag& active;
            ~CallbackReentryGuard() { active.clear(std::memory_order_release); }
        } callbackReentryGuard{callbackActive_};
#endif
        // 样本更新闸门：更新期间输出静音
        RealtimeSection section;
        if (!section.TryEnter(loading_, activeCallbacks_)) {
            WriteSilence(out, numFrames);
            return;
        }
        if (state_.load(std::memory_order_acquire) != State::Running) {
            WriteSilence(out, numFrames);
            return;
        }

        const int requestedSampleRate = requestedSampleRate_.load(std::memory_order_relaxed);
        if (beat::IsValidSampleRate(requestedSampleRate) &&
            requestedSampleRate != appliedSampleRate_) {
            clock_.SetSampleRate(requestedSampleRate);
            appliedSampleRate_ = requestedSampleRate;
        }

        const std::uint64_t startGeneration =
                startGeneration_.load(std::memory_order_acquire);
        if (startGeneration != appliedStartGeneration_) {
            clock_.SetBPM(bpm_.load(std::memory_order_relaxed));
            appliedStartGeneration_ = startGeneration;
        }

        // 一次性加载参数（relaxed 足够，单生产者单消费者）
        double bpm = bpm_.load(std::memory_order_relaxed);
        if (std::abs(bpm - clock_.GetBPM()) > 1e-9) {
            clock_.SetBPMPreservePhase(bpm);  // 运行中调 BPM：保相位连续
        }
        float outGain = static_cast<float>(outputGain_.load(std::memory_order_relaxed));
        float tickGain = static_cast<float>(tickGain_.load(std::memory_order_relaxed)) * outGain;
        float chimeGain = static_cast<float>(chimeGain_.load(std::memory_order_relaxed)) * outGain;
        int accentMode = accentMode_.load(std::memory_order_relaxed);

        const unsigned chimeCount = pendingChimeTriggers_.exchange(0, std::memory_order_acquire);
        for (unsigned i = 0; i < chimeCount; ++i) {
            chimePlayer_.Play();
        }
        // 每拍回调注册信息一次性快照（seqlock），循环内只用局部变量
        const TickCallbackSnapshot tickCb = LoadTickCallback();
        // 两路 player 都要推进以让长尾自然结束，只混入当前拍对应的一路
        for (int i = 0; i < numFrames; ++i) {
            if (clock_.Advance()) {
                const std::uint64_t beatSeq =
                        beatSeq_.fetch_add(1, std::memory_order_relaxed) + 1;
                if (tickCb.callback != nullptr) {
                    tickCb.callback(tickCb.userData, beatSeq,
                                    framesRendered_.load(std::memory_order_relaxed)
                                            + static_cast<std::uint64_t>(i),
                                    appliedSampleRate_);
                }
                currentHeavy_ = IsAccentBeat(accentMode, beatSeq);
                if (currentHeavy_) {
                    tickPlayer_.Play();
                } else {
                    tickLoPlayer_.Play();
                }
            }
            float hiSample  = tickPlayer_.ReadOne();
            float loSample  = tickLoPlayer_.ReadOne();
            float tickSample = currentHeavy_ ? hiSample : loSample;
            float chimeSample = chimePlayer_.ReadOne();
            out[i] = tickSample * tickGain + chimeSample * chimeGain;
        }
        framesRendered_.fetch_add(static_cast<std::uint64_t>(numFrames),
                                  std::memory_order_relaxed);

        Limiter::ProcessHard(out, numFrames);
    }

private:
    [[nodiscard]] static bool IsAccentBeat(int mode, std::uint64_t beatSeq) noexcept {
        switch (static_cast<AccentMode>(mode)) {
            case AccentMode::AllHeavy:    return true;
            case AccentMode::AllLight:    return false;
            case AccentMode::Every2:      return (beatSeq % 2) == 1;
            case AccentMode::Every2Light: return (beatSeq % 2) != 1;
            case AccentMode::Every4:      return (beatSeq % 4) == 1;
            case AccentMode::Every4Light: return (beatSeq % 4) != 1;
        }
        return true;
    }

    /** 每拍回调注册信息的只读快照（seqlock 保证 callback 与 userData 成对）。 */
    struct TickCallbackSnapshot {
        TickCallback callback;
        void* userData;
    };

    [[nodiscard]] TickCallbackSnapshot LoadTickCallback() const noexcept {
        for (int attempt = 0; attempt < 4; ++attempt) {
            const unsigned seq = tickCallbackSeq_.load(std::memory_order_acquire);
            if ((seq & 1u) != 0u) continue;   // 写入中：重试
            TickCallback callback = tickCallback_.load(std::memory_order_acquire);
            void* userData = tickCallbackUserData_.load(std::memory_order_acquire);
            if (tickCallbackSeq_.load(std::memory_order_acquire) == seq) {
                return {callback, userData};
            }
        }
        return {nullptr, nullptr};   // 连续竞争：跳过本 buffer 的回调，不阻塞实时线程
    }

    /** RAII 样本更新闸门：音频线程拒绝进入（否则等自己而死锁），并保证 loading_ 复位。 */
    class SampleUpdateScope {
    public:
        explicit SampleUpdateScope(AudioEngine& engine) noexcept
            : engine_(engine), entered_(engine.BeginSampleUpdate()) {}
        ~SampleUpdateScope() noexcept {
            if (entered_) engine_.EndSampleUpdate();
        }
        SampleUpdateScope(const SampleUpdateScope&) = delete;
        SampleUpdateScope& operator=(const SampleUpdateScope&) = delete;
        [[nodiscard]] bool Entered() const noexcept { return entered_; }

    private:
        AudioEngine& engine_;
        bool entered_;
    };

    [[nodiscard]] bool BeginSampleUpdate() noexcept {
        // 实时线程上直接拒绝：它会等 activeCallbacks_ 归零，也就是等自己；
        // 不能用 assert，abort 会拖死整机音频
        if (inAudioCallback_) return false;
        while (sampleUpdateLock_.test_and_set(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        loading_.store(true, std::memory_order_seq_cst);
        while (activeCallbacks_.load(std::memory_order_seq_cst) != 0) {
            std::this_thread::yield();
        }
        return true;
    }

    void EndSampleUpdate() noexcept {
        loading_.store(false, std::memory_order_seq_cst);
        sampleUpdateLock_.clear(std::memory_order_release);
    }

    Clock clock_;
    SamplePlayer tickPlayer_;
    SamplePlayer tickLoPlayer_;
    SamplePlayer chimePlayer_;

    std::atomic<bool> loading_{false};
    std::atomic<unsigned> activeCallbacks_{0};
    std::atomic_flag sampleUpdateLock_ = ATOMIC_FLAG_INIT;
    std::atomic_flag callbackActive_ = ATOMIC_FLAG_INIT;
    std::atomic<unsigned> pendingChimeTriggers_{0};

    std::atomic<double> bpm_{beat::kDefaultBpm};
    std::atomic<std::uint64_t> startGeneration_{0};
    // 最新值请求；callback 之间的多次采样率更新会合并为最后一个值。
    std::atomic<int> requestedSampleRate_{beat::kDefaultSampleRate};
    // 仅由 OnAudioCallback 所在线程访问。
    std::uint64_t appliedStartGeneration_ = 0;
    int appliedSampleRate_ = beat::kDefaultSampleRate;
    std::atomic<double> tickGain_{beat::kDefaultTickGain};
    std::atomic<double> chimeGain_{beat::kDefaultChimeGain};
    std::atomic<int> accentMode_{static_cast<int>(AccentMode::AllHeavy)};
    // 当前拍用 hi 还是 lo：触发时更新，尾音期间保持不变
    bool currentHeavy_ = true;
    std::atomic<double> outputGain_{beat::kDefaultOutputGain};

    std::atomic<State> state_{State::Idle};

    std::atomic<std::uint64_t> beatSeq_{0};
    std::atomic<std::uint64_t> framesRendered_{0};
    // 每拍回调注册：控制线程用 seqlock 发布，音频线程快照读取
    std::atomic<TickCallback> tickCallback_{nullptr};
    std::atomic<void*> tickCallbackUserData_{nullptr};
    std::atomic<unsigned> tickCallbackSeq_{0};

    // 仅音频回调线程为 true；供 Load* 拒绝在实时线程上做样本更新。
    static inline thread_local bool inAudioCallback_ = false;
};
