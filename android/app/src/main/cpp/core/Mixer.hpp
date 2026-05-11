#pragma once

/**
 * 混音器 — 无状态双声道混音。
 *
 * 公式：out[i] = tickBuf[i] * tickGain + chimeBuf[i] * chimeGain
 *
 * 纯头文件，内联优化，音频线程安全。
 */
struct Mixer {
    /** 混音：tick 和 chime 两路信号按增益混合到输出 */
    static void Process(const float* tickBuf, const float* chimeBuf,
                        float* out, int numFrames,
                        float tickGain, float chimeGain) noexcept {
        for (int i = 0; i < numFrames; ++i) {
            out[i] = tickBuf[i] * tickGain + chimeBuf[i] * chimeGain;
        }
    }
};
