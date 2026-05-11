#pragma once

#include <cstdint>
#include <vector>
#include <cstring>
#include <cmath>

/**
 * 简易 WAV 解析器 — 仅支持 16-bit PCM Mono。
 *
 * 支持：采样率任意，自动重采样至 targetSampleRate。
 * 输入格式：标准 RIFF WAV（PCM, 16-bit, mono）。
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
                int16_t* pcmData = reinterpret_cast<int16_t*>(
                    const_cast<uint8_t*>(wavData) + pos);
                int numSamples = static_cast<int>(chunkSize / 2);
                rawPcm.assign(pcmData, pcmData + numSamples);
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
    /** 生成指数衰减正弦波 WAV 文件数据 */
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
};
