#pragma once

/**
 * 共享常量 — 引擎默认值与魔法数字的唯一定义点。
 * 各平台桥接（cpp/aaudio_engine.cpp、ios/.../AudioEngineBridge.mm）应从这里取值，
 * 避免在多处复制字面量导致漂移。
 */
namespace beat {

/// 默认/目标采样率（硬件协商失败或协商前使用；流打开后会同步为实际值）
constexpr int kDefaultSampleRate = 48000;

/// 默认 tick 增益（x1 输出下，与 ±0.6 裁剪的默认木鱼组合峰值约 ±0.42）
constexpr double kDefaultTickGain = 0.7;

/// 默认 chime 增益
constexpr double kDefaultChimeGain = 0.5;

/// 输出总增益默认值（x1）
constexpr double kDefaultOutputGain = 1.0;

/// 硬限幅阈值（保留 3% headroom，保持 click 瞬态清脆）
constexpr float kHardLimit = 0.97f;

/// 默认 BPM
constexpr double kDefaultBpm = 180.0;

}  // namespace beat
