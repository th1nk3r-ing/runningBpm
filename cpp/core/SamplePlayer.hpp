#pragma once

#include <cstdint>
#include <vector>
#include <atomic>

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
    void Load(const float* data, size_t length) noexcept {
        sampleData_.assign(data, data + length);
        readPos_.store(0, std::memory_order_release);
        playing_.store(false, std::memory_order_release);
    }

    /** 从当前读位置开始播放 */
    void Play() noexcept {
        readPos_.store(0, std::memory_order_release);
        playing_.store(true, std::memory_order_release);
    }

    /** 停止播放 */
    void Stop() noexcept {
        playing_.store(false, std::memory_order_release);
        readPos_.store(0, std::memory_order_release);
    }

    /** 获取播放状态 */
    [[nodiscard]] bool IsPlaying() const noexcept { return playing_.load(std::memory_order_acquire); }

    /**
     * 读取一个采样并前进读指针，实时安全。
     * @return 当前采样值，播放完毕返回 0
     */
    float ReadOne() noexcept {
        if (!playing_.load(std::memory_order_acquire)) return 0.0f;
        size_t pos = readPos_.load(std::memory_order_acquire);
        if (pos < sampleData_.size()) {
            readPos_.store(pos + 1, std::memory_order_release);
            return sampleData_[pos];
        }
        playing_.store(false, std::memory_order_release);
        return 0.0f;
    }

    /**
     * 渲染 numFrames 帧到 out 缓冲区。
     * 播放中时逐帧输出 sampleData_[readPos_++] * gain，
     * 播放完毕或未播放时输出静音。
     */
    void Render(float* out, int numFrames, float gain) noexcept {
        if (!playing_.load(std::memory_order_acquire)) {
            for (int i = 0; i < numFrames; ++i) out[i] = 0.0f;
            return;
        }

        for (int i = 0; i < numFrames; ++i) {
            size_t pos = readPos_.load(std::memory_order_acquire);
            if (pos < sampleData_.size()) {
                readPos_.store(pos + 1, std::memory_order_release);
                out[i] = sampleData_[pos] * gain;
            } else {
                out[i] = 0.0f;
                playing_.store(false, std::memory_order_release);
            }
        }
    }

private:
    std::vector<float> sampleData_;
    std::atomic<size_t> readPos_{0};
    std::atomic<bool> playing_{false};
};
