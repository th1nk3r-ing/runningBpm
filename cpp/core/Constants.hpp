#pragma once

#include <algorithm>
#include <cmath>

/** 引擎默认值与魔法数字的唯一定义点 — 各桥接从这里取值，避免字面量漂移。 */
namespace beat {

// 硬件协商前使用；流打开后同步为实际值
constexpr int kDefaultSampleRate = 48000;

// x1 输出下，与 ±0.6 裁剪的默认木鱼组合峰值约 ±0.42
constexpr double kDefaultTickGain = 0.7;

constexpr double kDefaultChimeGain = 0.5;

constexpr double kDefaultOutputGain = 1.0;

// 保留 3% headroom，保持 click 瞬态清脆
constexpr float kHardLimit = 0.97f;

constexpr double kDefaultBpm = 180.0;

// 越界会让 framesPerTick 退化（< 1 帧或极大），导致每帧触发或永不触发；所有入口统一夹取
constexpr double kMinBpm = 30.0;
constexpr double kMaxBpm = 300.0;

// getSampleRate 对无效流可能返回 0，会让 BPM 更新静默失效，必须拦截
constexpr int kMinSampleRate = 8000;
constexpr int kMaxSampleRate = 384000;

/** 非法/NaN 输入回落到默认值，其余夹取到 [kMinBpm, kMaxBpm]。 */
inline double SanitizeBpm(double bpm) noexcept {
    if (!std::isfinite(bpm)) return kDefaultBpm;
    return std::clamp(bpm, kMinBpm, kMaxBpm);
}

[[nodiscard]] inline bool IsValidSampleRate(int sampleRate) noexcept {
    return sampleRate >= kMinSampleRate && sampleRate <= kMaxSampleRate;
}

}  // namespace beat
