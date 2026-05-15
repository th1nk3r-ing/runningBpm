#pragma once

/**
 * 混音器 — 无状态双路信号混音（tick + chime → mono）。
 *
 * 公式：out[i] = tickBuf[i] * tickGain + chimeBuf[i] * chimeGain
 * 注意：这是两个信号源混合到单声道输出，不是立体声（stereo）双声道。
 *
 * 纯头文件，内联优化，音频线程安全。
 */
struct Mixer {
    /** 混音：tick 和 chime 两路信号按各自增益叠加到单声道输出 */
    static void Process(const float* tickBuf, const float* chimeBuf,
                        float* out, int numFrames,
                        float tickGain, float chimeGain) noexcept {
        for (int i = 0; i < numFrames; ++i) {
            out[i] = tickBuf[i] * tickGain + chimeBuf[i] * chimeGain;
        }
    }
};
