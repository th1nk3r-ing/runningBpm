#pragma once

#include <cstdint>
#include <vector>

/**
 * 采样播放器 — 事件驱动的单触发采样播放。
 *
 * 由外部事件调用 Play() 触发播放，Render() 逐采样拷贝 PCM 数据，
 * 播放完毕后自动静音。音频回调中仅调用 Render()，不触发任何控制逻辑。
 *
 * 线程安全：非音频线程调用 Load()/Play()，音频线程调用 Render()。
 *          调用方需确保 Load/Play 与 Render 互斥。
 */
class SamplePlayer {
public:
    /** 加载 PCM 样本数据 */
    void Load(const float* data, size_t length) noexcept {
        sampleData_.assign(data, data + length);
        readPos_ = 0;
        playing_ = false;
    }

    /** 从当前读位置开始播放 */
    void Play() noexcept {
        readPos_ = 0;
        playing_ = true;
    }

    /** 停止播放 */
    void Stop() noexcept {
        playing_ = false;
        readPos_ = 0;
    }

    /** 获取播放状态 */
    [[nodiscard]] bool IsPlaying() const noexcept { return playing_; }

    /**
     * 渲染 numFrames 帧到 out 缓冲区。
     * 播放中时逐帧输出 sampleData_[readPos_++] * gain，
     * 播放完毕或未播放时输出静音。
     */
    void Render(float* out, int numFrames, float gain) noexcept {
        if (!playing_) {
            for (int i = 0; i < numFrames; ++i) out[i] = 0.0f;
            return;
        }

        for (int i = 0; i < numFrames; ++i) {
            if (readPos_ < sampleData_.size()) {
                out[i] = sampleData_[readPos_++] * gain;
            } else {
                out[i] = 0.0f;
                playing_ = false;
            }
        }
    }

private:
    std::vector<float> sampleData_;
    uint32_t readPos_ = 0;
    bool playing_ = false;
};
