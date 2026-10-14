#pragma once

#include <cmath>

#include "Constants.hpp"

/** 限幅器 — 防止多声源叠加导致数字削波；生产用硬限幅，软限幅仅测试备选。 */
struct Limiter {
    /** tanh 软限幅（输出趋近 ±1.0）— test-only，生产只用 ProcessHard。 */
    static void Process(float* buffer, int numFrames) noexcept {
        for (int i = 0; i < numFrames; ++i) {
            buffer[i] = std::tanh(buffer[i]);
        }
    }

    /** 硬限幅（峰值 > ±0.97 时介入，保留 3% headroom，保持 click 瞬态清脆） */
    static void ProcessHard(float* buffer, int numFrames) noexcept {
        constexpr float kLimit = beat::kHardLimit;
        for (int i = 0; i < numFrames; ++i) {
            float x = buffer[i];
            buffer[i] = (x > kLimit) ? kLimit : ((x < -kLimit) ? -kLimit : x);
        }
    }
};
