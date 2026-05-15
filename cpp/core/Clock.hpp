#pragma once

#include <vector>

/**
 * 倒计数式节拍时钟 — 跨 buffer 维护连续倒计数，消除累积量化误差。
 *
 * 采用 countdown 方式：每采样递减 framesToNextTick_，
 * 归零时触发 tick 并加上 framesPerTick_。
 * 此方式将浮点误差约束在每个 tick 内，不跨 tick 累积。
 *
 * 注意：与相位累加器（phase += deltaPhi；phase >= 1.0 时触发）在数学上等价，
 *       framesPerTick = 1/deltaPhi（单位：帧），但直接以帧数倒计数更直观易验证。
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

    /**
     * 设置采样率（音频路由变更时调用）。
     * 保持当前相位百分比连续：新 framesPerTick 下剩余比例与旧值相同，
     * 避免采样率切换（如蓝牙耳机接入）导致下一拍提前或推迟。
     */
    void SetSampleRate(int sr) noexcept {
        if (sampleRate_ == sr) return;
        // 保存旧相位比例（剩余帧数 / 总帧数）
        double ratio = (framesPerTick_ > 0.0) ? (framesToNextTick_ / framesPerTick_) : 1.0;
        sampleRate_ = sr;
        UpdateFramesPerTick();                        // 会重置 framesToNextTick_ = framesPerTick_
        framesToNextTick_ = framesPerTick_ * ratio;   // 用旧比例修正，保持相位连续
    }

    /**
     * 设置 BPM 并重置相位（framesToNextTick_ = framesPerTick_）。
     * 用于初始化 / Stop 后重启等"对齐到第一拍"的场景。
     * 运行中调整 BPM 请用 SetBPMPreservePhase，否则会让下一拍提前/推迟。
     */
    void SetBPM(double bpm) noexcept {
        bpm_ = bpm;
        UpdateFramesPerTick();
        framesToNextTick_ = framesPerTick_;
    }

    /**
     * 设置 BPM 但保持当前相位百分比连续 — 运行中调整 BPM 必用此函数。
     * 思路：当前 countdown 在旧 framesPerTick 中的"已走百分比"在新 framesPerTick
     * 中保持不变，避免因重置 countdown 导致的"提前一拍"听感。
     *
     * 例：旧 fpt=24000、当前 framesToNextTick=6000（已走 75%）→ 新 fpt=20000
     *     则新 framesToNextTick = 20000 * (6000/24000) = 5000（仍为 75%）
     */
    void SetBPMPreservePhase(double bpm) noexcept {
        if (bpm <= 0.0 || sampleRate_ <= 0) return;
        double oldFpt = framesPerTick_;
        bpm_ = bpm;
        framesPerTick_ = static_cast<double>(sampleRate_) * 60.0 / bpm_;
        if (oldFpt > 0.0) {
            // 保持"剩余比例"不变
            framesToNextTick_ = framesPerTick_ * (framesToNextTick_ / oldFpt);
        } else {
            framesToNextTick_ = framesPerTick_;
        }
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
     * 单步前进一帧，实时安全。
     * @return true 如果本帧发生 tick 事件
     */
    bool Advance() noexcept {
        framesToNextTick_ -= 1.0;
        if (framesToNextTick_ <= 0.0) {
            framesToNextTick_ += framesPerTick_;
            return true;
        }
        return false;
    }

    /**
     * 处理 numFrames 个采样点，返回发生 tick 事件的帧索引数组。
     * 注意：Process 内部堆分配，仅用于测试/离线处理，
     *       音频回调中请使用 Advance()。
     * @param numFrames  本次回调的帧数
     * @return 所有发生 tick 的帧索引（0 ~ numFrames-1），已排序
     */
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
    double bpm_ = 180.0;
    int sampleRate_ = 48000;
};
