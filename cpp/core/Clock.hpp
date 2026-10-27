#pragma once

#include <algorithm>
#include <vector>

#include "Constants.hpp"

/**
 * 倒计数式节拍时钟 — 每采样递减 framesToNextTick_，归零触发并加回
 * framesPerTick_（= sampleRate*60/BPM），等价于相位累加器；误差只在单 tick 内，
 * 不跨 tick 累积。
 *
 * BPM / sampleRate 入口统一夹取，避免 framesPerTick_ 退化成 0 或 <1。
 */
class Clock {
public:
    explicit Clock(int sampleRate = beat::kDefaultSampleRate) noexcept
        : sampleRate_(beat::IsValidSampleRate(sampleRate) ? sampleRate : 0) {
        UpdateFramesPerTick();
    }

    /** 音频路由变更时按旧相位比例重算剩余帧，避免下一拍提前/推迟；非法值忽略。 */
    void SetSampleRate(int sr) noexcept {
        if (!beat::IsValidSampleRate(sr)) return;
        if (sampleRate_ == sr) return;
        double ratio = (framesPerTick_ > 0.0)
                               ? std::clamp(framesToNextTick_ / framesPerTick_, 0.0, 1.0)
                               : 1.0;
        sampleRate_ = sr;
        UpdateFramesPerTick();
        framesToNextTick_ = framesPerTick_ * ratio;   // 用旧比例修正，保持相位连续
    }

    /** 设置 BPM 并重置相位（初始化 / Stop 后重启）；运行中调整请用 SetBPMPreservePhase。 */
    void SetBPM(double bpm) noexcept {
        bpm_ = beat::SanitizeBpm(bpm);
        UpdateFramesPerTick();
        framesToNextTick_ = framesPerTick_;
    }

    /** 运行中设置 BPM 并保持相位连续。 */
    void SetBPMPreservePhase(double bpm) noexcept {
        if (framesPerTick_ <= 0.0) return;            // 尚无有效采样率
        const double oldFpt = framesPerTick_;
        bpm_ = beat::SanitizeBpm(bpm);
        framesPerTick_ = std::max(1.0, static_cast<double>(sampleRate_) * 60.0 / bpm_);
        // ratio 理论上 ∈ [0,1]；夹取兜住历史异常状态（例如曾出现负剩余帧）。
        const double ratio = std::clamp(framesToNextTick_ / oldFpt, 0.0, 1.0);
        framesToNextTick_ = framesPerTick_ * ratio;
    }

    [[nodiscard]] double GetBPM() const noexcept { return bpm_; }

    /** test-only：仅 clock_preserve_phase_test 用它断言相位状态。 */
    [[nodiscard]] double GetFramesToNextTick() const noexcept { return framesToNextTick_; }

    void Reset() noexcept {
        framesToNextTick_ = framesPerTick_;
    }

    /** 单步前进一帧，实时安全；返回本帧是否触发 tick。 */
    bool Advance() noexcept {
        if (framesPerTick_ <= 0.0) return false;   // 无有效采样率：不产生节拍
        framesToNextTick_ -= 1.0;
        if (framesToNextTick_ <= 0.0) {
            // do/while 兜住 framesPerTick_ 曾 <1 留下的负剩余帧，否则会每帧都触发
            do {
                framesToNextTick_ += framesPerTick_;
            } while (framesToNextTick_ <= 0.0);
            return true;
        }
        return false;
    }

    /** 处理 numFrames 帧，返回发生 tick 的帧索引；test-only，内部堆分配、实时不安全。 */
    [[nodiscard]] std::vector<int> Process(int numFrames) noexcept {
        std::vector<int> ticks;
        if (numFrames <= 0) return ticks;
        if (framesPerTick_ > 0.0) {
            ticks.reserve(static_cast<size_t>(static_cast<double>(numFrames) / framesPerTick_) + 1);
        }

        for (int i = 0; i < numFrames; ++i) {
            if (Advance()) {
                ticks.push_back(i);
            }
        }

        return ticks;
    }

private:
    void UpdateFramesPerTick() noexcept {
        if (!beat::IsValidSampleRate(sampleRate_)) {
            framesPerTick_ = 0.0;
            framesToNextTick_ = 0.0;
            return;
        }
        bpm_ = beat::SanitizeBpm(bpm_);
        framesPerTick_ = std::max(1.0, static_cast<double>(sampleRate_) * 60.0 / bpm_);
        framesToNextTick_ = framesPerTick_;
    }

    double framesPerTick_ = 0.0;
    double framesToNextTick_ = 0.0;
    double bpm_ = beat::kDefaultBpm;
    int sampleRate_ = beat::kDefaultSampleRate;
};
