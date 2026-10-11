#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

#include "Clock.hpp"
#include "SamplePlayer.hpp"
#include "Limiter.hpp"

/**
 * 音频引擎状态机 — 整合 Clock、SamplePlayer、Limiter 为完整 Pipeline。
 *
 * 线程模型：
 *   - 控制线程（Main/JNI）：Start/Stop/Pause/Resume/SetBpm/SetVolume/TriggerChime
 *   - 音频线程（AAudio callback）：OnAudioCallback（必须实时安全）
 *   参数通过 std::atomic 无锁交换，音频线程在 callback 入口处一次加载。
 *   Clock 状态仅由 callback 线程访问；Start / SetSampleRate 通过原子请求应用。
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
        // Clock 由音频线程独占。下一个 callback 会看到 generation 变化并重置时钟。
        startGeneration_.fetch_add(1, std::memory_order_release);
        state_.store(State::Running, std::memory_order_release);
    }

    void Stop() noexcept {
        state_.store(State::Idle, std::memory_order_release);
        tickPlayer_.Stop();
        tickLoPlayer_.Stop();
        chimePlayer_.Stop();
    }

    void Pause() noexcept {
        state_.store(State::Paused, std::memory_order_relaxed);
    }

    void Resume() noexcept {
        if (state_.load(std::memory_order_relaxed) == State::Paused) {
            state_.store(State::Running, std::memory_order_relaxed);
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
        requestedSampleRate_.store(sr, std::memory_order_release);
    }

    void TriggerChime() noexcept {
        chimePlayer_.Play();
    }

    [[nodiscard]] State GetState() const noexcept {
        return state_.load(std::memory_order_relaxed);
    }

    // ===== 样本加载（启动前调用） =====

    void LoadTickSamples(const float* data, size_t length) noexcept {
        tickPlayer_.Load(data, length);
    }

    void LoadTickLoSamples(const float* data, size_t length) noexcept {
        tickLoPlayer_.Load(data, length);
    }

    void LoadChimeSamples(const float* data, size_t length) noexcept {
        chimePlayer_.Load(data, length);
    }

    // ===== 音频回调（实时线程，严禁阻塞/分配） =====

    /**
     * 运行时替换 tick 样本（控制线程调用）。
     * 使用 seq_cst 保证：音频线程若看到 loading_=false，必定也看到完整的新样本数据。
     */
    void LoadSoundPack(const float* tickHi, size_t tickHiLen,
                        const float* tickLo, size_t tickLoLen) noexcept {
        loading_.store(true, std::memory_order_seq_cst);   // ① 先设标志（seq_cst 全屏障）
        tickPlayer_.Load(tickHi, tickHiLen);                // ② 写新样本
        tickLoPlayer_.Load(tickLo, tickLoLen);
        loading_.store(false, std::memory_order_seq_cst);  // ③ 清除标志（seq_cst 全屏障）
    }

    void OnAudioCallback(float* out, int numFrames) noexcept {
        if (loading_.load(std::memory_order_seq_cst)) {    // seq_cst 确保看到最新值
            for (int i = 0; i < numFrames; ++i) out[i] = 0.0f;
            return;
        }
        if (state_.load(std::memory_order_acquire) != State::Running) {
            for (int i = 0; i < numFrames; ++i) out[i] = 0.0f;
            return;
        }

        // Clock 的全部运行时访问均发生在 callback 线程。
        const int requestedSampleRate = requestedSampleRate_.load(std::memory_order_acquire);
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

        // 每帧推进两个 player 让长尾自然结束，只混入 accent 对应的一路。
        // 注意：accent 关闭时，所有 tick 触发都使用 tickLoPlayer_（弱拍/单一音色）。
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
    Clock clock_;
    SamplePlayer tickPlayer_;
    SamplePlayer tickLoPlayer_;
    SamplePlayer chimePlayer_;

    std::atomic<bool> loading_{false};

    std::atomic<double> bpm_{180.0};
    std::atomic<std::uint64_t> startGeneration_{0};
    std::atomic<int> requestedSampleRate_{48000};
    // 仅由 OnAudioCallback 所在线程访问。
    std::uint64_t appliedStartGeneration_ = 0;
    int appliedSampleRate_ = 48000;
    std::atomic<double> tickGain_{0.7};  // 预留限幅余量
    std::atomic<double> chimeGain_{0.5};
    std::atomic<bool> accentOn_{true};
    // 输出总倍率，叠加在 tick/chime 增益之上
    std::atomic<double> outputGain_{1.0};

    std::atomic<State> state_{State::Idle};
};
