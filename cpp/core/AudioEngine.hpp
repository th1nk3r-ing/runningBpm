#pragma once

#include <atomic>
#include <cmath>

#include "Clock.hpp"
#include "SamplePlayer.hpp"
#include "Limiter.hpp"

/**
 * 音频引擎状态机 — 整合 Clock、SamplePlayer、Mixer、Limiter 为完整 Pipeline。
 *
 * 线程模型：
 *   - 控制线程（Main/JNI）：Start/Stop/Pause/Resume/SetBpm/SetVolume/TriggerChime
 *   - 音频线程（AAudio callback）：OnAudioCallback（必须实时安全）
 *   参数通过 std::atomic 无锁交换，音频线程在 callback 入口处一次加载。
 *
 * Pipeline 各帧：
 *   1. Clock.Advance()                   → 是否触发 tick
 *   2. tickPlayer.Play() / .ReadOne()    → tick 采样
 *   3. chimePlayer.ReadOne()             → chime 采样
 *   4. 混合：out[i] = tick * gain + chime * gain
 *   5. Limiter::Process()                → tanh 软限幅
 */
class AudioEngine {
public:
    enum class State : int { Idle, Running, Paused };

    AudioEngine() = default;
    ~AudioEngine() { Stop(); }

    // ===== 控制接口（非音频线程调用） =====

    void Start(double bpm) noexcept {
        bpm_.store(bpm, std::memory_order_relaxed);
        state_.store(State::Running, std::memory_order_relaxed);
    }

    void Stop() noexcept {
        state_.store(State::Idle, std::memory_order_relaxed);
        tickPlayer_.Stop();
        tickLoPlayer_.Stop();
        chimePlayer_.Stop();
        clock_.Reset();
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

    /** 同步 AAudio 实际采样率到 Clock */
    void SetSampleRate(int sr) noexcept {
        clock_.SetSampleRate(sr);
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

    /** 运行时替换 tick 样本（线程安全，需先设置 loading 标志） */
    void LoadSoundPack(const float* tickHi, size_t tickHiLen,
                        const float* tickLo, size_t tickLoLen) noexcept {
        loading_.store(true, std::memory_order_relaxed);
        tickPlayer_.Load(tickHi, tickHiLen);
        tickLoPlayer_.Load(tickLo, tickLoLen);
        loading_.store(false, std::memory_order_relaxed);
    }

    void OnAudioCallback(float* out, int numFrames) noexcept {
        if (loading_.load(std::memory_order_relaxed)) {
            for (int i = 0; i < numFrames; ++i) out[i] = 0.0f;
            return;
        }
        if (state_.load(std::memory_order_relaxed) != State::Running) {
            for (int i = 0; i < numFrames; ++i) out[i] = 0.0f;
            return;
        }

        // 一次性加载参数（relaxed 足够，单生产者单消费者）
        double bpm = bpm_.load(std::memory_order_relaxed);
        if (std::abs(bpm - clock_.GetBPM()) > 1e-9) {
            clock_.SetBPM(bpm);
        }
        float outGain = static_cast<float>(outputGain_.load(std::memory_order_relaxed));
        float tickGain = static_cast<float>(tickGain_.load(std::memory_order_relaxed)) * outGain;
        float chimeGain = static_cast<float>(chimeGain_.load(std::memory_order_relaxed)) * outGain;
        bool accent = accentOn_.load(std::memory_order_relaxed);

        // Per-sample 处理：始终推进所有 player（让长尾自然播放完），
        // 仅在 tick 触发时启动对应的 player。
        // 注意：accent 关闭时，所有 tick 触发都使用 tickLoPlayer_（弱拍/单一音色）。
        for (int i = 0; i < numFrames; ++i) {
            if (clock_.Advance()) {
                if (accent) {
                    tickPlayer_.Play();
                } else {
                    tickLoPlayer_.Play();
                }
            }
            float tickSample = tickPlayer_.ReadOne() + tickLoPlayer_.ReadOne();
            float chimeSample = chimePlayer_.ReadOne();
            out[i] = tickSample * tickGain + chimeSample * chimeGain;
        }

        // 使用硬限幅（只在峰值 > 0.97 时介入），保持瞬态线性度。
        // 之前的 tanh 软限幅在 |x|>0.3 时已开始压缩，会让 click 听起来"闷"。
        Limiter::ProcessHard(out, numFrames);
    }

private:
    Clock clock_;
    SamplePlayer tickPlayer_;
    SamplePlayer tickLoPlayer_;
    SamplePlayer chimePlayer_;

    std::atomic<bool> loading_{false};

    std::atomic<double> bpm_{180.0};
    // 默认 tick 音量略降到 0.7：click 波形峰值约 0.55，乘 0.7 后峰值 ~0.39，
    // 远低于硬限幅 0.97 阈值，可保持瞬态完全线性。
    std::atomic<double> tickGain_{0.7};
    std::atomic<double> chimeGain_{0.5};
    std::atomic<bool> accentOn_{true};
    // 输出总倍率（x1/x2/x3 等），叠加在 tickGain/chimeGain 之上
    std::atomic<double> outputGain_{1.0};

    std::atomic<State> state_{State::Idle};
};
