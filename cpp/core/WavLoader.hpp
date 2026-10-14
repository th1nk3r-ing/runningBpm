#pragma once

#include <cassert>
#include <cstdint>
#include <vector>
#include <cstring>
#include <cmath>
#include <limits>

/**
 * 简易 WAV 解析器。
 *
 * 支持格式：标准 RIFF WAV，PCM 16-bit mono，采样率任意。
 * 要求 RIFF/chunk 声明不越过实际数据范围，奇数长度 chunk 带 RIFF padding；拒绝截断或占位长度文件。
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

        // RIFF header
        if (wavData == nullptr || dataSize < 44) return false;
        if (Read32(wavData, 0) != 0x46464952) return false; // "RIFF"
        const uint32_t fileSize = Read32(wavData, 4);
        if (Read32(wavData, 8) != 0x45564157) return false; // "WAVE"
        if (fileSize < 4 || static_cast<size_t>(fileSize) > dataSize - 8) return false;
        const size_t riffEnd = 8 + static_cast<size_t>(fileSize);
        size_t pos = 12;

        // Chunks
        int sampleRate = 0;
        int bitsPerSample = 0;
        std::vector<int16_t> rawPcm;

        while (pos < riffEnd) {
            if (riffEnd - pos < 8) return false;
            const uint32_t chunkId = Read32(wavData, pos);
            const uint32_t chunkSize = Read32(wavData, pos + 4);
            const size_t payloadPos = pos + 8;
            if (static_cast<size_t>(chunkSize) > riffEnd - payloadPos) return false;
            size_t nextPos = payloadPos + static_cast<size_t>(chunkSize);
            if ((chunkSize & 1U) != 0) {
                if (nextPos >= riffEnd) return false;
                ++nextPos; // RIFF chunks are padded to an even byte boundary.
            }

            if (chunkId == 0x20746D66) { // "fmt "
                if (chunkSize < 16) return false;
                uint16_t audioFormat = Read16(wavData, payloadPos);
                uint16_t numChannels = Read16(wavData, payloadPos + 2);
                uint32_t parsedSampleRate = Read32(wavData, payloadPos + 4);
                if (parsedSampleRate == 0 ||
                    parsedSampleRate > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
                    return false;
                }
                sampleRate = static_cast<int>(parsedSampleRate);
                bitsPerSample = Read16(wavData, payloadPos + 14);

                if (audioFormat != 1) return false; // PCM only
                if (numChannels != 1) return false; // Mono only
                if (bitsPerSample != 16) return false; // 16-bit only

            } else if (chunkId == 0x61746164) { // "data"
                if ((chunkSize & 1U) != 0) return false; // PCM16 requires complete samples.
                const size_t numSamples = static_cast<size_t>(chunkSize) / 2;
                rawPcm.resize(numSamples);
                for (size_t i = 0; i < numSamples; ++i) {
                    rawPcm[i] = static_cast<int16_t>(
                        wavData[payloadPos + i * 2]
                        | (static_cast<uint16_t>(wavData[payloadPos + i * 2 + 1]) << 8));
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

    /**
     * 线性重采样（公用：采样率变化后重新重采样已加载样本）。
     * @param inRate 输入采样率；outRate 输出采样率。非正值或相等率（恒等）时
     *   原样返回输入，避免 ratio=inf/NaN —— 非法输入视作防御性失败而非崩溃。
     */
    static std::vector<float> Resample(
            const std::vector<float>& input, int inRate, int outRate) {
        if (inRate <= 0 || outRate <= 0) {
            assert(inRate > 0 && outRate > 0 && "Resample requires positive sample rates");
            return input;
        }
        if (inRate == outRate) return input;

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

    std::vector<float> samples_;
    int sampleRate_ = 0;
};

// ===== WAV 文件生成器（构建时使用） =====

struct WavGenerator {
    /** 将 float PCM 采样数据编码为 16-bit mono WAV 文件字节流 */
    static std::vector<uint8_t> Generate(const std::vector<float>& samples,
                                          int sampleRate) {
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

    /**
     * 生成"塑料/木块"敲击 click：强拍 ~3.2kHz/45ms，弱拍 ~2.4kHz/30ms。
     * 0.4ms 淡入与 2ms 淡出抑制边界突变；波形裁剪到 ±0.6，避免默认增益下触及硬限幅。
     *
     * @param accent  true=强拍（更高频 + 稍长 + 略大）；false=弱拍
     */
    static std::vector<float> MakeWoodTick(int sampleRate, bool accent) noexcept {
        const float durationSec = accent ? 0.045f : 0.030f;
        const float fundamental = accent ? 3200.0f : 2400.0f;
        const float decay       = accent ? 95.0f   : 130.0f;
        const float clickAmp    = accent ? 0.55f   : 0.42f;
        const float noiseLenSec = 0.0008f;
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

        // 极短噪声瞬态（仅前 0.8ms）
        unsigned seed = accent ? 1337u : 7331u;
        for (int i = 0; i < noiseLen && i < length; ++i) {
            seed = seed * 1664525u + 1013904223u;
            float n = static_cast<float>((seed >> 8) & 0xFFFF) / 32768.0f - 1.0f;
            float ne = 1.0f - static_cast<float>(i) / noiseLen;
            data[i] += n * noiseAmp * ne;
        }

        // Attack 淡入 / 末尾淡出（消除数字突变与截断咔声）
        for (int i = 0; i < fadeIn && i < length; ++i) {
            data[i] *= static_cast<float>(i) / fadeIn;
        }

        for (int i = 0; i < fadeOut && (length - 1 - i) >= 0; ++i) {
            data[length - 1 - i] *= static_cast<float>(i) / fadeOut;
        }

        // 裁剪到 ±0.6，低于硬限幅阈值 ±0.97
        for (int i = 0; i < length; ++i) {
            if (data[i] > 0.6f)  data[i] = 0.6f;
            if (data[i] < -0.6f) data[i] = -0.6f;
        }

        return data;
    }
};
