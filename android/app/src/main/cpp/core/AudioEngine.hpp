#pragma once

#include <atomic>

// Phase 2.6: 音频引擎状态机（当前为占位骨架）
class AudioEngine {
public:
    enum class State { Idle, Running, Paused };

    void Start(double /*bpm*/) {}
    void Stop() {}
    void Pause() {}
    void Resume() {}
    void SetBpm(double /*bpm*/) {}
    void SetTickVolume(double /*vol*/) {}
    void SetChimeVolume(double /*vol*/) {}
    void SetAccent(bool /*on*/) {}
    void TriggerChime() {}

    [[nodiscard]] State GetState() const { return state_; }

private:
    State state_ = State::Idle;
};
