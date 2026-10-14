#pragma once

#include <vector>

#include "Constants.hpp"

/**
 * 倒计数式节拍时钟 — 每采样递减 framesToNextTick_，归零触发 tick 并加回
 * framesPerTick_（= sampleRate*60/BPM）。浮点误差约束在单 tick 内、不跨 tick
 * 累积；数学上等价于相位累加器（framesPerTick = 1/Δφ）。
 */
class Clock {
public:
    explicit Clock(int sampleRate = beat::kDefaultSampleRate) noexcept
        : sampleRate_(sampleRate) {
        UpdateFramesPerTick();
    }

    /** 设置采样率（音频路由变更时调用）。按旧相位比例重算剩余帧，避免切换
     *  （如蓝牙耳机接入）导致下一拍提前或推迟。 */
    void SetSampleRate(int sr) noexcept {
        if (sampleRate_ == sr) return;
        double ratio = (framesPerTick_ > 0.0) ? (framesToNextTick_ / framesPerTick_) : 1.0;
        sampleRate_ = sr;
        UpdateFramesPerTick();
        framesToNextTick_ = framesPerTick_ * ratio;   // 用旧比例修正，保持相位连续
    }

    /** 设置 BPM 并重置相位 — 用于初始化 / Stop 后重启（对齐到第一拍）。
     *  运行中调整 BPM 请用 SetBPMPreservePhase，否则下一拍会提前/推迟。 */
    void SetBPM(double bpm) noexcept {
        bpm_ = bpm;
        UpdateFramesPerTick();
        framesToNextTick_ = framesPerTick_;
    }

    /** 设置 BPM 并保持相位连续 — 运行中调整 BPM 必用此函数
     *  （新 framesToNextTick_ = 新 fpt × 旧剩余 / 旧 fpt）。 */
    void SetBPMPreservePhase(double bpm) noexcept {
        if (bpm <= 0.0 || sampleRate_ <= 0) return;
        double oldFpt = framesPerTick_;
        bpm_ = bpm;
        framesPerTick_ = static_cast<double>(sampleRate_) * 60.0 / bpm_;
        if (oldFpt > 0.0) {
            framesToNextTick_ = framesPerTick_ * (framesToNextTick_ / oldFpt);
        } else {
            framesToNextTick_ = framesPerTick_;
        }
    }

    [[nodiscard]] double GetBPM() const noexcept { return bpm_; }

    /** test-only：仅 clock_preserve_phase_test 用它断言相位状态。 */
    [[nodiscard]] double GetFramesToNextTick() const noexcept { return framesToNextTick_; }

    void Reset() noexcept {
        framesToNextTick_ = framesPerTick_;
    }

    /** 单步前进一帧，实时安全；返回本帧是否触发 tick。 */
    bool Advance() noexcept {
        framesToNextTick_ -= 1.0;
        if (framesToNextTick_ <= 0.0) {
            framesToNextTick_ += framesPerTick_;
            return true;
        }
        return false;
    }

    /** 处理 numFrames 帧，返回发生 tick 的帧索引（已排序）。
     *  test-only — 生产回调用 Advance()；本函数内部堆分配，实时不安全。 */
    [[nodiscard]] std::vector<int> Process(int numFrames) noexcept {
        std::vector<int> ticks;
        ticks.reserve(static_cast<size_t>(numFrames / framesPerTick_) + 1);

        for (int i = 0; i < numFrames; ++i) {
            if (Advance()) {
                ticks.push_back(i);
            }
        }

        return ticks;
    }

private:
    void UpdateFramesPerTick() noexcept {
        if (bpm_ > 0.0 && sampleRate_ > 0) {
            double clampedBpm = bpm_;
            framesPerTick_ = static_cast<double>(sampleRate_) * 60.0 / clampedBpm;
            framesToNextTick_ = framesPerTick_;
        }
    }

    double framesPerTick_ = 0.0;
    double framesToNextTick_ = 0.0;
    double bpm_ = beat::kDefaultBpm;
    int sampleRate_ = beat::kDefaultSampleRate;
};
