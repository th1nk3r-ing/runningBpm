#pragma once

#include <cstdint>
#include <vector>
#include <atomic>
#include <array>

/**
 * 复音采样播放器 — Voice Pool + Double Buffer。
 * Play() 分配空闲（或最先结束的）voice，避免新触发截断长音色；ReadOne() 累加
 * 所有活跃 voice，后级 Limiter 保护。Load() 写非活跃缓冲后翻转索引，音频线程
 * 读活跃缓冲 —— Load 与 ReadOne/Render 不得并发。
 * 线程：控制线程 Load/Play/Stop；音频线程 ReadOne/Render。
 */
class SamplePlayer {
public:
    static constexpr int kVoices = 4;

    /** 加载新样本（控制线程）：写非活跃缓冲，完成后翻转索引。经 AudioEngine
     *  调用时由引擎保证当前 callback 已退出。 */
    void Load(const float* data, size_t length) noexcept {
        int inactive = 1 - activeIdx_.load(std::memory_order_relaxed);
        bufs_[inactive].assign(data, data + length);
        // seq_cst store：发布已写入的缓冲区索引（并发 Load/Read 由调用方排除）
        activeIdx_.store(inactive, std::memory_order_seq_cst);
        for (auto& v : voices_) {
            v.readPos.store(0, std::memory_order_release);
            v.playing.store(false, std::memory_order_release);
        }
    }

    /** 触发一次播放：分配空闲 voice；若全部繁忙，复用读位置最大者（最先结束） */
    void Play() noexcept {
        if (bufs_[activeIdx_.load(std::memory_order_seq_cst)].empty()) return;

        int target = -1;
        for (int i = 0; i < kVoices; ++i) {
            if (!voices_[i].playing.load(std::memory_order_acquire)) {
                target = i;
                break;
            }
        }
        if (target < 0) {
            size_t maxPos = 0;
            target = 0;
            for (int i = 0; i < kVoices; ++i) {
                size_t p = voices_[i].readPos.load(std::memory_order_acquire);
                if (p >= maxPos) {
                    maxPos = p;
                    target = i;
                }
            }
        }
        voices_[target].readPos.store(0, std::memory_order_release);
        voices_[target].playing.store(true, std::memory_order_release);
    }

    void Stop() noexcept {
        for (auto& v : voices_) {
            v.playing.store(false, std::memory_order_release);
            v.readPos.store(0, std::memory_order_release);
        }
    }

    [[nodiscard]] bool IsPlaying() const noexcept {
        for (const auto& v : voices_) {
            if (v.playing.load(std::memory_order_acquire)) return true;
        }
        return false;
    }

    /** 读取一个采样并前进所有活跃 voice（实时安全），返回各 voice 采样之和。
     *  Load() 与读取不得并发。 */
    float ReadOne() noexcept {
        // seq_cst load：若 Load() 已完成 seq_cst store(inactive)，此处必然读到新索引
        const auto& data = bufs_[activeIdx_.load(std::memory_order_seq_cst)];
        if (data.empty()) return 0.0f;
        float acc = 0.0f;
        const size_t len = data.size();
        for (auto& v : voices_) {
            if (!v.playing.load(std::memory_order_acquire)) continue;
            size_t pos = v.readPos.load(std::memory_order_acquire);
            if (pos < len) {
                acc += data[pos];
                v.readPos.store(pos + 1, std::memory_order_release);
            } else {
                v.playing.store(false, std::memory_order_release);
            }
        }
        return acc;
    }

    /** 渲染 numFrames 帧到 out（带 gain）— test-only，生产内联用 ReadOne()。 */
    void Render(float* out, int numFrames, float gain) noexcept {
        for (int i = 0; i < numFrames; ++i) {
            out[i] = ReadOne() * gain;
        }
    }

private:
    struct Voice {
        std::atomic<size_t> readPos{0};
        std::atomic<bool> playing{false};
    };
    // double buffer：控制线程写非活跃缓冲，音频线程读活跃缓冲
    std::vector<float> bufs_[2];
    std::atomic<int> activeIdx_{0};
    std::array<Voice, kVoices> voices_;
};
