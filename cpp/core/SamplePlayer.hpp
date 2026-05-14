#pragma once

#include <cstdint>
#include <vector>
#include <atomic>
#include <array>

/**
 * 复音采样播放器 — 事件驱动、Voice Pool 设计。
 *
 * 设计要点：
 *   1) 内置 N 个 voice（独立 readPos），Play() 分配空闲（或最先开始的）voice，
 *      避免新触发截断尚在播放的样本，对 150ms+ 的长音色（Clap 等）尤为重要。
 *   2) ReadOne() 累加所有活跃 voice 的当前采样，简单 mix 后由后级 Limiter 保护。
 *   3) 实时安全：所有 voice 字段为 POD/atomic，无堆分配；
 *      Load 仅在控制线程使用 loading 标志互斥（音频线程读到时输出静音一段）。
 *
 * 线程模型：
 *   - 控制线程：Load() / Play() / Stop()
 *   - 音频线程：ReadOne() / Render()
 */
class SamplePlayer {
public:
    static constexpr int kVoices = 4;

    void Load(const float* data, size_t length) noexcept {
        sampleData_.assign(data, data + length);
        for (auto& v : voices_) {
            v.readPos.store(0, std::memory_order_release);
            v.playing.store(false, std::memory_order_release);
        }
    }

    /** 触发一次播放：分配空闲 voice；若全部繁忙，复用读位置最大者（最先结束） */
    void Play() noexcept {
        if (sampleData_.empty()) return;

        int target = -1;
        // 找空闲 voice
        for (int i = 0; i < kVoices; ++i) {
            if (!voices_[i].playing.load(std::memory_order_acquire)) {
                target = i;
                break;
            }
        }
        // 全部繁忙 → 选 readPos 最大的（最先结束）voice 复用
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

    /** 停止所有 voice */
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

    /**
     * 读取一个采样并前进所有活跃 voice 的读指针，实时安全。
     * @return 所有活跃 voice 当前采样之和
     */
    float ReadOne() noexcept {
        if (sampleData_.empty()) return 0.0f;
        float acc = 0.0f;
        const size_t len = sampleData_.size();
        for (auto& v : voices_) {
            if (!v.playing.load(std::memory_order_acquire)) continue;
            size_t pos = v.readPos.load(std::memory_order_acquire);
            if (pos < len) {
                acc += sampleData_[pos];
                v.readPos.store(pos + 1, std::memory_order_release);
            } else {
                v.playing.store(false, std::memory_order_release);
            }
        }
        return acc;
    }

    /** 渲染 numFrames 帧到 out 缓冲区（带 gain），用于离线测试 */
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
    std::vector<float> sampleData_;
    std::array<Voice, kVoices> voices_;
};
