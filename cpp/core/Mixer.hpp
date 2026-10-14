#pragma once

/** 双路混音（tick + chime → mono）— test-only，生产在 OnAudioCallback 内联混音。 */
struct Mixer {
    static void Process(const float* tickBuf, const float* chimeBuf,
                        float* out, int numFrames,
                        float tickGain, float chimeGain) noexcept {
        for (int i = 0; i < numFrames; ++i) {
            out[i] = tickBuf[i] * tickGain + chimeBuf[i] * chimeGain;
        }
    }
};
