#pragma once

#include <cmath>

/**
 * 限幅器 — 防止多声源叠加导致的数字削波。
 *
 * 提供两种实现：
 *   Process     — tanh 软限幅：小信号线性直通（tanh(x) ≈ x for |x| ≪ 1），
 *                 大信号渐进饱和趋近 ±1.0（无固定 headroom，不适合强调瞬态的 click 音色）。
 *   ProcessHard — 硬限幅：超过 ±0.97 才介入（保留 3% headroom），
 *                 峰值以下完全线性，更适合保留 click 瞬态清脆感。
 *
 * AudioEngine 当前使用 ProcessHard，以保持节拍 click 的瞬态线性度。
 */
struct Limiter {
    /** tanh 软限幅（渐进饱和，输出趋近 ±1.0，适合音调信号） */
    static void Process(float* buffer, int numFrames) noexcept {
        for (int i = 0; i < numFrames; ++i) {
            buffer[i] = std::tanh(buffer[i]);
        }
    }

    /** 硬限幅（峰值 > ±0.97 时介入，保留 3% headroom，保持 click 瞬态清脆） */
    static void ProcessHard(float* buffer, int numFrames) noexcept {
        constexpr float kLimit = 0.97f;
        for (int i = 0; i < numFrames; ++i) {
            float x = buffer[i];
            buffer[i] = (x > kLimit) ? kLimit : ((x < -kLimit) ? -kLimit : x);
        }
    }
};
