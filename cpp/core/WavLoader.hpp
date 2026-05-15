#pragma once

#include <cstdint>
#include <vector>
#include <cstring>
#include <cmath>

/**
 * 简易 WAV 解析器。
 *
 * 支持格式：标准 RIFF WAV，PCM 16-bit mono，采样率任意。
 * 不支持：stereo、24/32-bit、IEEE float、压缩格式。
 * 重采样：若 targetSampleRate > 0 且与文件采样率不同，自动线性重采样至目标采样率。
 *
 * 用法：
 *   WavLoader loader;
 *   if (loader.Load(data, size, targetSampleRate)) {
 *       auto& samples = loader.GetSamples();  // float [-1, 1]
 *   }
 */

class WavLoader {
public:
    bool Load(const uint8_t* wavData, size_t dataSize, int targetSampleRate = 0) {
        Reset();

        size_t pos = 0;

        // RIFF header
        if (dataSize < 44) return false;
        if (Read32(wavData, pos) != 0x46464952) return false; // "RIFF"
        pos += 4;
        uint32_t fileSize = Read32(wavData, pos); pos += 4;
        if (Read32(wavData, pos) != 0x45564157) return false; // "WAVE"
        pos += 4;

        // Chunks
        int sampleRate = 0;
        int bitsPerSample = 0;
        std::vector<int16_t> rawPcm;

        while (pos + 8 <= dataSize) {
            uint32_t chunkId = Read32(wavData, pos); pos += 4;
            uint32_t chunkSize = Read32(wavData, pos); pos += 4;
            uint32_t nextPos = pos + chunkSize;

            if (chunkId == 0x20746D66) { // "fmt "
                if (chunkSize < 16) return false;
                uint16_t audioFormat = Read16(wavData, pos);
                uint16_t numChannels = Read16(wavData, pos + 2);
                sampleRate = static_cast<int>(Read32(wavData, pos + 4));
                bitsPerSample = Read16(wavData, pos + 14);

                if (audioFormat != 1) return false; // PCM only
                if (numChannels != 1) return false; // Mono only
                if (bitsPerSample != 16) return false; // 16-bit only

            } else if (chunkId == 0x61746164) { // "data"
                // clamp 实际可读字节数，防止 chunkSize 超出文件末尾时越界访问
                size_t availableBytes = (pos + chunkSize <= dataSize)
                                        ? chunkSize
                                        : (dataSize - pos);
                int numSamples = static_cast<int>(availableBytes / 2);
                rawPcm.resize(numSamples);
                for (int i = 0; i < numSamples; ++i) {
                    rawPcm[i] = static_cast<int16_t>(
                        wavData[pos + i * 2]
                        | (static_cast<uint16_t>(wavData[pos + i * 2 + 1]) << 8));
                }
            }

            pos = nextPos;
        }

        if (rawPcm.empty() || sampleRate == 0) return false;

        // 转为 float [-1, 1]
        samples_.resize(rawPcm.size());
        for (size_t i = 0; i < rawPcm.size(); ++i) {
            samples_[i] = rawPcm[i] / 32768.0f;
        }

        // 重采样
        if (targetSampleRate > 0 && targetSampleRate != sampleRate) {
            samples_ = Resample(samples_, sampleRate, targetSampleRate);
        }

        sampleRate_ = (targetSampleRate > 0) ? targetSampleRate : sampleRate;
        return true;
    }

    [[nodiscard]] const std::vector<float>& GetSamples() const { return samples_; }
    [[nodiscard]] int GetSampleRate() const { return sampleRate_; }

private:
    void Reset() {
        samples_.clear();
        sampleRate_ = 0;
    }

    static uint32_t Read32(const uint8_t* data, size_t pos) {
        return static_cast<uint32_t>(data[pos])
             | (static_cast<uint32_t>(data[pos + 1]) << 8)
             | (static_cast<uint32_t>(data[pos + 2]) << 16)
             | (static_cast<uint32_t>(data[pos + 3]) << 24);
    }

    static uint16_t Read16(const uint8_t* data, size_t pos) {
        return static_cast<uint16_t>(data[pos])
             | (static_cast<uint16_t>(data[pos + 1]) << 8);
    }

    /** 线性重采样 */
    static std::vector<float> Resample(
            const std::vector<float>& input, int inRate, int outRate) {
        double ratio = static_cast<double>(inRate) / outRate;
        size_t outLen = static_cast<size_t>(input.size() / ratio);
        std::vector<float> output(outLen);

        for (size_t i = 0; i < outLen; ++i) {
            double srcPos = i * ratio;
            size_t srcIdx = static_cast<size_t>(srcPos);
            double frac = srcPos - srcIdx;

            if (srcIdx + 1 < input.size()) {
                output[i] = static_cast<float>(
                    input[srcIdx] * (1.0 - frac) + input[srcIdx + 1] * frac);
            } else {
                output[i] = input[srcIdx];
            }
        }
        return output;
    }

    std::vector<float> samples_;
    int sampleRate_ = 0;
};

// ===== WAV 文件生成器（构建时使用） =====

struct WavGenerator {
    /** 将 float PCM 采样数据编码为 16-bit mono WAV 文件字节流 */
    static std::vector<uint8_t> Generate(const std::vector<float>& samples,
                                          int sampleRate) {
        // 转为 16-bit PCM
        std::vector<int16_t> pcm(samples.size());
        for (size_t i = 0; i < samples.size(); ++i) {
            float s = std::max(-1.0f, std::min(1.0f, samples[i]));
            pcm[i] = static_cast<int16_t>(s * 32767.0f);
        }

        // WAV header
        uint32_t dataSize = static_cast<uint32_t>(pcm.size() * 2);
        uint32_t fileSize = 36 + dataSize;

        std::vector<uint8_t> wav(44 + dataSize);
        size_t pos = 0;

        auto write32 = [&](uint32_t v) {
            wav[pos++] = static_cast<uint8_t>(v);
            wav[pos++] = static_cast<uint8_t>(v >> 8);
            wav[pos++] = static_cast<uint8_t>(v >> 16);
            wav[pos++] = static_cast<uint8_t>(v >> 24);
        };
        auto write16 = [&](uint16_t v) {
            wav[pos++] = static_cast<uint8_t>(v);
            wav[pos++] = static_cast<uint8_t>(v >> 8);
        };

        // RIFF
        write32(0x46464952); // "RIFF"
        write32(fileSize);
        write32(0x45564157); // "WAVE"
        // fmt
        write32(0x20746D66); // "fmt "
        write32(16);         // chunk size
        write16(1);          // PCM
        write16(1);          // mono
        write32(sampleRate);
        write32(sampleRate * 2); // byte rate
        write16(2);              // block align
        write16(16);             // bits per sample
        // data
        write32(0x61746164); // "data"
        write32(dataSize);

        // PCM data
        for (size_t i = 0; i < pcm.size(); ++i) {
            write16(static_cast<uint16_t>(pcm[i]));
        }

        return wav;
    }

    /** 生成 tick 采样数据 */
    static std::vector<float> MakeTick(int sampleRate, float freq,
                                        float durationSec, float decay) {
        int length = static_cast<int>(sampleRate * durationSec);
        std::vector<float> data(length);
        for (int i = 0; i < length; ++i) {
            float t = static_cast<float>(i) / sampleRate;
            data[i] = std::sin(2.0f * 3.14159265f * freq * t)
                    * std::exp(-t * decay);
        }
        return data;
    }

    /**
     * 生成清脆的节拍 click —— 经过精心调音的"塑料/木块"敲击声。
     *
     * 设计要点：
     *   1) 短促尖锐：总长 30~45ms，强拍稍长，弱拍更短；
     *   2) 高频主导：强拍 ~3.2kHz，弱拍 ~2.4kHz，跑步耳机/外放都易听；
     *   3) 三段式包络：0.4ms 淡入（消除数字啪声）→ 主体快速指数衰减 → 末尾 2ms 线性淡出；
     *   4) 峰值控制 ≤ 0.55，避免后级 Limiter (tanh) 压缩造成失真；
     *   5) 极轻微高斯噪声"咔哒"（< 1ms）增加颗粒感，但不抢戏。
     *
     * @param accent  true=强拍（更高频 + 稍长 + 略大）；false=弱拍
     */
    static std::vector<float> MakeWoodTick(int sampleRate, bool accent) noexcept {
        const float durationSec = accent ? 0.045f : 0.030f;
        const float fundamental = accent ? 3200.0f : 2400.0f;
        const float decay       = accent ? 95.0f   : 130.0f;
        const float clickAmp    = accent ? 0.55f   : 0.42f;
        const float noiseLenSec = 0.0008f;   // 仅 0.8ms 噪声瞬态
        const float noiseAmp    = accent ? 0.18f : 0.14f;

        const int length    = static_cast<int>(sampleRate * durationSec);
        const int noiseLen  = static_cast<int>(sampleRate * noiseLenSec);
        const int fadeIn    = std::max(8, static_cast<int>(sampleRate * 0.0004f));
        const int fadeOut   = std::max(16, static_cast<int>(sampleRate * 0.002f));

        std::vector<float> data(length, 0.0f);

        // 主体：单一基频 + 轻微 1.5x 不和谐分量（金属感而非纯正弦）
        const float twoPi = 6.28318530718f;
        for (int i = 0; i < length; ++i) {
            float t = static_cast<float>(i) / sampleRate;
            float env = std::exp(-t * decay);
            float tone = std::sin(twoPi * fundamental * t)
                       + 0.25f * std::sin(twoPi * fundamental * 1.5f * t);
            data[i] = tone * clickAmp * env;
        }

        // 极短噪声瞬态（仅前 0.8ms）— 增加 "啪" 的颗粒，但不刺耳
        unsigned seed = accent ? 1337u : 7331u;
        for (int i = 0; i < noiseLen && i < length; ++i) {
            seed = seed * 1664525u + 1013904223u;
            float n = static_cast<float>((seed >> 8) & 0xFFFF) / 32768.0f - 1.0f;
            float ne = 1.0f - static_cast<float>(i) / noiseLen;
            data[i] += n * noiseAmp * ne;
        }

        // Attack 淡入（消除采样起点的数字突变）
        for (int i = 0; i < fadeIn && i < length; ++i) {
            data[i] *= static_cast<float>(i) / fadeIn;
        }

        // 末尾淡出（消除截断"咔"声）
        for (int i = 0; i < fadeOut && (length - 1 - i) >= 0; ++i) {
            data[length - 1 - i] *= static_cast<float>(i) / fadeOut;
        }

        // 安全裁剪到 ±0.6（远低于 Limiter 拐点，保留瞬态线性度）
        for (int i = 0; i < length; ++i) {
            if (data[i] > 0.6f)  data[i] = 0.6f;
            if (data[i] < -0.6f) data[i] = -0.6f;
        }

        return data;
    }
};
