#pragma once

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

}  // namespace beat
