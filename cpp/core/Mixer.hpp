#pragma once

/**
 * 混音器 — 双路信号混音（tick + chime → mono，非立体声）。
 * test-only — 生产链路在 AudioEngine::OnAudioCallback 内联混音，不经过此组件。
 * 无状态、内联，实时安全。
 */
struct Mixer {
    static void Process(const float* tickBuf, const float* chimeBuf,
                        float* out, int numFrames,
                        float tickGain, float chimeGain) noexcept {
        for (int i = 0; i < numFrames; ++i) {
            out[i] = tickBuf[i] * tickGain + chimeBuf[i] * chimeGain;
        }
    }
};
