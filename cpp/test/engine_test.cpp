/**
 * AudioEngine 验收测试
 *
 * 模拟回调循环输出 10s 音频数据，写入 WAV 文件，
 * 验证节拍间隔均等。
 *
 * 编译：g++ -std=c++17 -O2 -I. -o engine_test engine_test.cpp && ./engine_test
 */

#include "../core/AudioEngine.hpp"
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <thread>
#include <vector>

// ===== WAV 写入 =====

struct WavHeader {
    char riff[4] = {'R', 'I', 'F', 'F'};
    uint32_t fileSize;
    char wave[4] = {'W', 'A', 'V', 'E'};
    char fmtId[4] = {'f', 'm', 't', ' '};
    uint32_t fmtSize = 16;
    uint16_t audioFormat = 3;  // IEEE float
    uint16_t numChannels = 1;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample = 32;
    char dataId[4] = {'d', 'a', 't', 'a'};
    uint32_t dataSize;
};

bool WriteWav(const std::string& path, const float* samples, size_t numSamples,
              int sampleRate) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;

    WavHeader hdr;
    hdr.sampleRate = static_cast<uint32_t>(sampleRate);
    hdr.byteRate = static_cast<uint32_t>(sampleRate * sizeof(float));
    hdr.blockAlign = sizeof(float);
    hdr.dataSize = static_cast<uint32_t>(numSamples * sizeof(float));
    hdr.fileSize = hdr.dataSize + sizeof(WavHeader) - 8;

    f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    f.write(reinterpret_cast<const char*>(samples), hdr.dataSize);
    return true;
}

// ===== 生成 tick 采样：440Hz 正弦波 × 指数衰减 =====

std::vector<float> GenerateTickSample(int sampleRate, float durationSec = 0.05f) {
    int length = static_cast<int>(sampleRate * durationSec);
    std::vector<float> data(length);
    for (int i = 0; i < length; ++i) {
        float t = static_cast<float>(i) / sampleRate;
        float envelope = std::exp(-t * 60.0f);
        data[i] = std::sin(2.0f * 3.14159f * 880.0f * t) * envelope;
    }
    return data;
}

// ===== 基于包络的 tick 检测 =====

struct TickAnalysis {
    std::vector<int> tickFrames;
    double meanInterval = 0;
    double maxJitter = 0;
    int totalTicks = 0;
    int expectedTicks = 0;
};

TickAnalysis AnalyzeTicks(const float* audio, int numFrames, int sampleRate,
                          double bpm) {
    TickAnalysis result;

    // 计算 RMS 包络（窗口 2ms）
    int windowSize = sampleRate / 500;  // 2ms
    if (windowSize < 1) windowSize = 1;
    std::vector<float> envelope(numFrames, 0.0f);
    for (int i = 0; i < numFrames; ++i) {
        float sq = audio[i] * audio[i];
        // 滑动均值
        if (i == 0) {
            envelope[i] = sq;
        } else {
            envelope[i] = envelope[i-1] + (sq - envelope[i-1]) / windowSize;
        }
    }

    // 从包络检测 onset：能量跳变超过动态范围
    float envMax = 0.0f;
    for (int i = 0; i < numFrames; ++i)
        if (envelope[i] > envMax) envMax = envelope[i];
    float threshold = envMax * 0.01f;  // 1%

    // 最小间隔 ≈ 1/4 节拍间隔（已知 BPM 时可用，但保守取 ~10ms 窗口）
    int minGap = sampleRate / 100;  // 10ms

    bool inOnset = false;
    for (int i = windowSize; i < numFrames; ++i) {
        // 能量上升超过阈值
        float rise = envelope[i] - envelope[i - windowSize];
        if (rise > threshold && !inOnset) {
            if (result.tickFrames.empty() ||
                i - result.tickFrames.back() >= minGap) {
                result.tickFrames.push_back(i);
            }
            inOnset = true;
        } else if (rise < threshold * 0.1f) {
            inOnset = false;
        }
    }

    result.totalTicks = static_cast<int>(result.tickFrames.size());
    result.expectedTicks = static_cast<int>(bpm * numFrames / (60.0 * sampleRate));

    if (result.tickFrames.size() > 1) {
        double sum = 0;
        double minInt = 1e9, maxInt = 0;
        for (size_t i = 1; i < result.tickFrames.size(); ++i) {
            double interval = result.tickFrames[i] - result.tickFrames[i - 1];
            sum += interval;
            if (interval < minInt) minInt = interval;
            if (interval > maxInt) maxInt = interval;
        }
        result.meanInterval = sum / (result.tickFrames.size() - 1);
        result.maxJitter = std::max(std::abs(result.meanInterval - minInt),
                                    std::abs(result.meanInterval - maxInt));
    }

    return result;
}

// Verify that sample-rate and Start requests are applied by the callback thread,
// and that every Start resets the first-tick phase using the latest settings.
bool TestDeferredClockUpdates() {
    constexpr float kImpulse[] = {1.0f};
    AudioEngine engine;
    engine.LoadTickSamples(kImpulse, 1);
    engine.SetTickVolume(1.0);
    engine.SetSampleRate(24000);

    auto firstSignalFrame = [](const std::vector<float>& audio) {
        for (size_t i = 0; i < audio.size(); ++i) {
            if (audio[i] != 0.0f) return static_cast<int>(i);
        }
        return -1;
    };

    std::vector<float> firstBeat(24000, 0.0f);
    engine.Start(60.0);
    engine.OnAudioCallback(firstBeat.data(), static_cast<int>(firstBeat.size()));
    const int firstStartFrame = firstSignalFrame(firstBeat);

    engine.Stop();
    std::vector<float> restartedBeat(12000, 0.0f);
    engine.Start(120.0);
    engine.OnAudioCallback(restartedBeat.data(), static_cast<int>(restartedBeat.size()));
    const int restartFrame = firstSignalFrame(restartedBeat);
    engine.Stop();

    const bool passed = firstStartFrame == 23999 && restartFrame == 11999;
    printf("Deferred clock updates: first=%d (expected 23999), restart=%d (expected 11999): %s\n",
           firstStartFrame, restartFrame, passed ? "PASS" : "FAIL");
    return passed;
}

// Stress control requests while a separate thread continuously renders callbacks.
// Run this test under ThreadSanitizer to verify Clock remains callback-thread-owned.
bool TestConcurrentClockControl() {
    AudioEngine engine;
    std::atomic<bool> render{true};
    std::atomic<bool> finiteOutput{true};
    std::atomic<int> callbackCount{0};

    std::thread callbackThread([&] {
        float output[128];
        while (render.load(std::memory_order_relaxed)) {
            engine.OnAudioCallback(output, 128);
            for (float sample : output) {
                if (!std::isfinite(sample)) {
                    finiteOutput.store(false, std::memory_order_relaxed);
                }
            }
            callbackCount.fetch_add(1, std::memory_order_relaxed);
        }
    });

    while (callbackCount.load(std::memory_order_relaxed) == 0) {
        std::this_thread::yield();
    }

    for (int i = 0; i < 20000; ++i) {
        engine.SetSampleRate((i & 1) ? 44100 : 48000);
        engine.Start(120.0 + static_cast<double>(i % 61));
        if (i % 3 == 0) {
            engine.Pause();
            engine.Resume();
        } else {
            engine.Stop();
        }
    }

    render.store(false, std::memory_order_relaxed);
    callbackThread.join();
    engine.Stop();
    const bool passed = finiteOutput.load(std::memory_order_relaxed);
    printf("Concurrent clock control: callbacks=%d, finite output: %s\n",
           callbackCount.load(std::memory_order_relaxed), passed ? "PASS" : "FAIL");
    return passed;
}

// ===== 主测试 =====

int main() {
    const bool clockUpdatesOk = TestDeferredClockUpdates();
    const bool concurrentClockControlOk = TestConcurrentClockControl();
    constexpr int kSampleRate = 48000;
    constexpr double kBPM = 150.0;
    constexpr int kDurationSec = 10;
    constexpr int kNumFrames = kSampleRate * kDurationSec;
    constexpr int kBufferSize = 256;

    // 生成 tick 采样
    auto tickData = GenerateTickSample(kSampleRate);
    printf("Tick sample: %zu frames (%.1f ms)\n",
           tickData.size(), tickData.size() * 1000.0 / kSampleRate);

    // 初始化引擎
    AudioEngine engine;
    engine.LoadTickSamples(tickData.data(), tickData.size());
    engine.Start(kBPM);

    // 生成音频
    std::vector<float> audio(kNumFrames);
    int framesGenerated = 0;

    while (framesGenerated < kNumFrames) {
        int chunk = std::min(kBufferSize, kNumFrames - framesGenerated);
        engine.OnAudioCallback(audio.data() + framesGenerated, chunk);
        framesGenerated += chunk;
    }

    engine.Stop();

    // 写入 WAV
    std::string wavPath = "/tmp/runbeat_engine_test.wav";
    if (WriteWav(wavPath, audio.data(), kNumFrames, kSampleRate)) {
        printf("WAV written: %s\n", wavPath.c_str());
    }

    // 分析 tick 间隔
    auto analysis = AnalyzeTicks(audio.data(), kNumFrames, kSampleRate, kBPM);

    printf("\n--- Tick Analysis ---\n");
    printf("BPM:                %.1f\n", kBPM);
    printf("Duration:           %d seconds\n", kDurationSec);
    printf("Expected ticks:     %d\n", analysis.expectedTicks);
    printf("Detected ticks:     %d\n", analysis.totalTicks);
    printf("Mean interval:      %.2f samples (%.4f sec)\n",
           analysis.meanInterval, analysis.meanInterval / kSampleRate);
    printf("Expected interval:  %.2f samples\n", 60.0 * kSampleRate / kBPM);

    // 打印前几个和间隔信息
    printf("\nFirst 5 tick frames:\n");
    for (int i = 0; i < std::min(5, analysis.totalTicks); ++i) {
        double sec = analysis.tickFrames[i] / static_cast<double>(kSampleRate);
        printf("  tick %d: frame %d (%.3fs)\n", i + 1, analysis.tickFrames[i], sec);
    }

    // 验收条件
    int tickDiff = std::abs(analysis.totalTicks - analysis.expectedTicks);
    bool countOk = tickDiff <= 1;
    bool jitterOk = analysis.maxJitter < 2.0;

    printf("\nTick count: %d (expected %d), diff=%d (≤1: %s)\n",
           analysis.totalTicks, analysis.expectedTicks,
           tickDiff, countOk ? "OK" : "FAIL");
    printf("Max jitter: %.4f samples (< 2: %s)\n",
           analysis.maxJitter, jitterOk ? "PASS" : "FAIL");

    printf("\n=== %s ===\n",
           (countOk && jitterOk && clockUpdatesOk && concurrentClockControlOk)
               ? "ALL PASS" : "FAILURES DETECTED");
    return (countOk && jitterOk && clockUpdatesOk && concurrentClockControlOk) ? 0 : 1;
}
