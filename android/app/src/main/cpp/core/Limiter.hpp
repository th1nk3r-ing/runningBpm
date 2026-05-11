#pragma once

#include <cmath>

/**
 * 软限幅器 — 防止多声源叠加导致的数字削波。
 *
 * 使用 tanh 实现平滑膝型限幅，保留 3% headroom。
 * 小信号线性直通（tanh(x) ≈ x for |x| ≪ 1），大信号平滑饱和。
 */
struct Limiter {
    static void Process(float* buffer, int numFrames) noexcept {
        for (int i = 0; i < numFrames; ++i) {
            buffer[i] = std::tanh(buffer[i]);
        }
    }

    /** 硬限幅备选（膝型更锐利，计算量更低） */
    static void ProcessHard(float* buffer, int numFrames) noexcept {
        constexpr float kLimit = 0.97f;
        for (int i = 0; i < numFrames; ++i) {
            float x = buffer[i];
            buffer[i] = (x > kLimit) ? kLimit : ((x < -kLimit) ? -kLimit : x);
        }
    }
};
