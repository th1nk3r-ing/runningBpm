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

/** 实时安全的静音填充 — callback 各静音出口共用。 */
inline void WriteSilence(float* out, int numFrames) noexcept {
    for (int i = 0; i < numFrames; ++i) out[i] = 0.0f;
}

/**
 * RAII 守卫：把 OnAudioCallback 注册为"活跃 callback"，保证
 * BeginSampleUpdate 能等待全部在途 callback 退出后再换样本。
 *
 * 内存序：
 *   - 入口两次 seq_cst 读 loading_ + 一次 seq_cst fetch_add/fetch_sub 发生在
 *     "看到 loading=true 之前"；第二个检查关闭了"更新在首次检查与发布计数之间
 *     开始"的竞争窗口。
 *   - TryEnter 成功后析构必然 seq_cst fetch_sub；失败路径（含第二次检查失败）
 *     已在 TryEnter 内自减，不会再减。
 */
class RealtimeSection {
public:
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
 * 音频引擎状态机 — 整合 Clock、SamplePlayer、Limiter 为完整 Pipeline。
 *
 * 线程模型：
 *   - 控制线程（Main/JNI）：Start/Stop/Pause/Resume/SetBpm/SetVolume/TriggerChime
 *   - 单一、串行的音频回调线程（AAudio/AudioUnit）：OnAudioCallback（必须实时安全）
 *   参数通过 std::atomic 无锁交换，音频线程在 callback 入口处一次加载。
 *   Clock 状态仅由 callback 线程访问；Start / SetSampleRate 通过原子请求应用。
 *   样本更新会等待所有在途 callback；Clock 和 voice 状态仍要求 callback 不重入。
 *   Debug 构建会在回调重入时断言失败。
 *
 * Pipeline 各帧：
 *   1. Clock.Advance()                    → 是否触发 tick
 *   2. tickPlayer.Play() / .ReadOne()     → tick 采样（hi 或 lo 由 accent 决定）
 *   3. chimePlayer.ReadOne()              → chime 采样
 *   4. 内联混音：out[i] = tick * tickGain + chime * chimeGain
 *   5. Limiter::ProcessHard()             → 硬限幅（±0.97）
 */
class AudioEngine {
public:
    enum class State : int { Idle, Running, Paused };

    AudioEngine() = default;
    ~AudioEngine() { Stop(); }

    // ===== 控制接口（非音频线程调用） =====

    void Start(double bpm) noexcept {
        bpm_.store(bpm, std::memory_order_relaxed);
        // Clock 由音频线程独占。generation 是合并式请求：callback 应用最新 BPM 和相位重置。
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
        bpm_.store(bpm, std::memory_order_relaxed);
    }

    void SetTickVolume(double vol) noexcept {
        tickGain_.store(vol, std::memory_order_relaxed);
    }

    void SetChimeVolume(double vol) noexcept {
        chimeGain_.store(vol, std::memory_order_relaxed);
    }

    void SetAccent(bool on) noexcept {
        accentOn_.store(on, std::memory_order_relaxed);
    }

    /** 输出总增益倍率（x1/x2/x3） — 经 Limiter 后再放大并 clamp，避免硬削波 */
    void SetOutputGain(double gain) noexcept {
        outputGain_.store(gain, std::memory_order_relaxed);
    }

    /** 请求更新采样率；实际 Clock 修改由下一次音频 callback 执行。 */
    void SetSampleRate(int sr) noexcept {
        requestedSampleRate_.store(sr, std::memory_order_relaxed);
    }

    void TriggerChime() noexcept {
        if (state_.load(std::memory_order_relaxed) != State::Running) return;
        // Voice state is callback-thread-owned. Queue a bounded number of requests
        // instead of calling SamplePlayer::Play() from the control thread.
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

    // ===== 样本加载（启动前调用） =====

    void LoadTickSamples(const float* data, size_t length) noexcept {
        BeginSampleUpdate();
        tickPlayer_.Load(data, length);
        EndSampleUpdate();
    }

    void LoadTickLoSamples(const float* data, size_t length) noexcept {
        BeginSampleUpdate();
        tickLoPlayer_.Load(data, length);
        EndSampleUpdate();
    }

    void LoadChimeSamples(const float* data, size_t length) noexcept {
        BeginSampleUpdate();
        chimePlayer_.Load(data, length);
        EndSampleUpdate();
    }

    // ===== 音频回调（实时线程，严禁阻塞/分配） =====

    /**
     * 运行时替换 tick 样本（控制线程调用）。
     * 在控制线程等待当前 callback 退出；更新期间的新 callback 输出静音。
     */
    void LoadSoundPack(const float* tickHi, size_t tickHiLen,
                        const float* tickLo, size_t tickLoLen) noexcept {
        BeginSampleUpdate();
        tickPlayer_.Load(tickHi, tickHiLen);
        tickLoPlayer_.Load(tickLo, tickLoLen);
        EndSampleUpdate();
    }

    void OnAudioCallback(float* out, int numFrames) noexcept {
#ifndef NDEBUG
        // Catch a host violating the documented single-serial-callback contract.
        if (callbackActive_.test_and_set(std::memory_order_acquire)) {
            assert(false && "AudioEngine::OnAudioCallback must not be reentrant");
            return;
        }
        struct CallbackReentryGuard {
            std::atomic_flag& active;
            ~CallbackReentryGuard() { active.clear(std::memory_order_release); }
        } callbackReentryGuard{callbackActive_};
#endif
        // 样本更新闸门：更新期间（或未能成功发布 callback 计数时）输出静音。
        RealtimeSection section;
        if (!section.TryEnter(loading_, activeCallbacks_)) {
            WriteSilence(out, numFrames);
            return;
        }
        if (state_.load(std::memory_order_acquire) != State::Running) {
            WriteSilence(out, numFrames);
            return;
        }

        // Clock 的全部运行时访问均发生在 callback 线程。
        const int requestedSampleRate = requestedSampleRate_.load(std::memory_order_relaxed);
        if (requestedSampleRate != appliedSampleRate_) {
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
        bool accent = accentOn_.load(std::memory_order_relaxed);

        const unsigned chimeCount = pendingChimeTriggers_.exchange(0, std::memory_order_acquire);
        for (unsigned i = 0; i < chimeCount; ++i) {
            chimePlayer_.Play();
        }
        // 每帧推进两个 player 让长尾自然结束，只混入 accent 对应的一路。
        for (int i = 0; i < numFrames; ++i) {
            if (clock_.Advance()) {
                if (accent) {
                    tickPlayer_.Play();
                } else {
                    tickLoPlayer_.Play();
                }
            }
            float hiSample  = tickPlayer_.ReadOne();
            float loSample  = tickLoPlayer_.ReadOne();
            float tickSample = accent ? hiSample : loSample;
            float chimeSample = chimePlayer_.ReadOne();
            out[i] = tickSample * tickGain + chimeSample * chimeGain;
        }

        // 硬限幅：仅峰值 > ±0.97 时介入，保留 click 瞬态
        Limiter::ProcessHard(out, numFrames);
    }

private:
    void BeginSampleUpdate() noexcept {
        // Serialize control-thread writers. This wait never occurs on the audio thread.
        while (sampleUpdateLock_.test_and_set(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        loading_.store(true, std::memory_order_seq_cst);
        while (activeCallbacks_.load(std::memory_order_seq_cst) != 0) {
            std::this_thread::yield();
        }
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
    // 默认生成 click 峰值裁剪到 ±0.6；x1 时乘 0.7 后不超过 ±0.42（硬限幅阈值 ±0.97）。
    std::atomic<double> tickGain_{beat::kDefaultTickGain};
    std::atomic<double> chimeGain_{beat::kDefaultChimeGain};
    std::atomic<bool> accentOn_{true};
    // 输出总倍率，叠加在 tick/chime 增益之上
    std::atomic<double> outputGain_{beat::kDefaultOutputGain};

    std::atomic<State> state_{State::Idle};
};
