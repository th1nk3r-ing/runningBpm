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

    void OnAudioCallback(float* out, int numFrames) noexcept {
        if (state_.load(std::memory_order_relaxed) != State::Running) {
            for (int i = 0; i < numFrames; ++i) out[i] = 0.0f;
            return;
        }

        // 一次性加载参数（relaxed 足够，单生产者单消费者）
        double bpm = bpm_.load(std::memory_order_relaxed);
        if (std::abs(bpm - clock_.GetBPM()) > 1e-9) {
            clock_.SetBPM(bpm);
        }
        float tickGain = static_cast<float>(tickGain_.load(std::memory_order_relaxed));
        float chimeGain = static_cast<float>(chimeGain_.load(std::memory_order_relaxed));
        bool accent = accentOn_.load(std::memory_order_relaxed);

        // Per-sample 处理
        for (int i = 0; i < numFrames; ++i) {
            if (clock_.Advance()) {
                if (accent) {
                    tickPlayer_.Play();
                } else {
                    tickLoPlayer_.Play();
                }
            }

            float s = (accent ? tickPlayer_ : tickLoPlayer_).ReadOne() * tickGain
                    + chimePlayer_.ReadOne() * chimeGain;
            out[i] = s;
        }

        Limiter::Process(out, numFrames);
    }

private:
    Clock clock_;
    SamplePlayer tickPlayer_;
    SamplePlayer tickLoPlayer_;
    SamplePlayer chimePlayer_;

    std::atomic<double> bpm_{180.0};
    std::atomic<double> tickGain_{0.8};
    std::atomic<double> chimeGain_{0.5};
    std::atomic<bool> accentOn_{true};

    std::atomic<State> state_{State::Idle};
};
