#pragma once

#include <cstdint>
#include <vector>
#include <atomic>
#include <array>

/**
 * 复音采样播放器 — 事件驱动、Voice Pool + Double Buffer 设计。
 *
 * 设计要点：
 *   1) 内置 N 个 voice（独立 readPos），Play() 分配空闲（或最先开始的）voice，
 *      避免新触发截断尚在播放的样本，对 150ms+ 的长音色（Clap 等）尤为重要。
 *   2) ReadOne() 累加所有活跃 voice 的当前采样，简单 mix 后由后级 Limiter 保护。
 *   3) Double buffer：Load() 写入非活跃缓冲区，seq_cst 原子翻转索引后音频线程读新缓冲。
 *      消除控制线程 assign() 与音频线程 ReadOne() 的 data race（标准 C++ UB）。
 *
 * 线程模型：
 *   - 控制线程：Load() / Play() / Stop()
 *   - 音频线程：ReadOne() / Render()
 */
class SamplePlayer {
public:
    static constexpr int kVoices = 4;

    /**
     * 加载新样本（控制线程调用）。
     * 写入非活跃缓冲区，完成后以 seq_cst store 翻转活跃索引，
     * 确保音频线程下次读取时看到完整的新数据。
     */
    void Load(const float* data, size_t length) noexcept {
        // 写入非活跃缓冲区（音频线程不会读它）
        int inactive = 1 - activeIdx_.load(std::memory_order_relaxed);
        bufs_[inactive].assign(data, data + length);
        // seq_cst store：确保 bufs_[inactive] 的写入对音频线程可见后再翻转索引
        activeIdx_.store(inactive, std::memory_order_seq_cst);
        // 停止所有 voice（旧样本的播放位置已无效）
        for (auto& v : voices_) {
            v.readPos.store(0, std::memory_order_release);
            v.playing.store(false, std::memory_order_release);
        }
    }

    /** 触发一次播放：分配空闲 voice；若全部繁忙，复用读位置最大者（最先结束） */
    void Play() noexcept {
        if (bufs_[activeIdx_.load(std::memory_order_seq_cst)].empty()) return;

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
     * 通过 seq_cst load activeIdx_ 获取当前活跃缓冲区，避免与 Load() 的 data race。
     * @return 所有活跃 voice 当前采样之和
     */
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
    // double buffer：控制线程写非活跃缓冲，音频线程读活跃缓冲
    std::vector<float> bufs_[2];
    std::atomic<int> activeIdx_{0};
    std::array<Voice, kVoices> voices_;
};
