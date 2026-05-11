#pragma once

#include <vector>

/**
 * 相位累加器 — 跨 buffer 维护连续相位，消除累积量化误差。
 *
 * 采用 countdown 方式：每采样递减 framesToNextTick_，
 * 归零时触发 tick 并加上 framesPerTick_。
 * 此方式将浮点误差约束在每个 tick 内，不跨 tick 累积。
 *
 * 核心公式：
 *   framesPerTick  = sampleRate * 60 / BPM
 *   每采样点 framesToNextTick_ -= 1.0
 *   当 framesToNextTick_ <= 0.0 时触发一次 tick
 */
class Clock {
public:
    explicit Clock(int sampleRate = 48000) noexcept
        : sampleRate_(sampleRate) {
        UpdateFramesPerTick();
    }

    /** 设置采样率（音频路由变更时调用） */
    void SetSampleRate(int sr) noexcept {
        sampleRate_ = sr;
        UpdateFramesPerTick();
    }

    /** 设置 BPM，更新每 tick 帧数 */
    void SetBPM(double bpm) noexcept {
        bpm_ = bpm;
        UpdateFramesPerTick();
    }

    /** 获取当前 BPM */
    [[nodiscard]] double GetBPM() const noexcept { return bpm_; }

    /** 获取当前帧计数（只读，用于状态恢复） */
    [[nodiscard]] double GetFramesToNextTick() const noexcept { return framesToNextTick_; }

    /** 设置帧计数（用于 Resume 时保持连续） */
    void SetFramesToNextTick(double f) noexcept { framesToNextTick_ = f; }

    /** 重置归零 */
    void Reset() noexcept {
        framesToNextTick_ = framesPerTick_;
    }

    /**
     * 处理 numFrames 个采样点，返回发生 tick 事件的帧索引数组。
     * @param numFrames  本次回调的帧数
     * @return 所有发生 tick 的帧索引（0 ~ numFrames-1），已排序
     */
    [[nodiscard]] std::vector<int> Process(int numFrames) noexcept {
        std::vector<int> ticks;
        ticks.reserve(static_cast<size_t>(numFrames / framesPerTick_) + 1);

        for (int i = 0; i < numFrames; ++i) {
            framesToNextTick_ -= 1.0;
            if (framesToNextTick_ <= 0.0) {
                ticks.push_back(i);
                framesToNextTick_ += framesPerTick_;
            }
        }

        return ticks;
    }

private:
    void UpdateFramesPerTick() noexcept {
        if (bpm_ > 0.0 && sampleRate_ > 0) {
            framesPerTick_ = static_cast<double>(sampleRate_) * 60.0 / bpm_;
            framesToNextTick_ = framesPerTick_;  // 第一个 tick 发生在一个完整间隔后
        }
    }

    double framesPerTick_ = 0.0;
    double framesToNextTick_ = 0.0;
    double bpm_ = 180.0;
    int sampleRate_ = 48000;
};
