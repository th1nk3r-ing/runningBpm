/**
 * Clock.hpp 验收测试
 *
 * 构造 BPM=150, sampleRate=48000，输入 4800000 步（100 秒），
 * 验证累积相位误差 < 1e-15。
 *
 * 编译：g++ -std=c++17 -O2 -o clock_test clock_test.cpp && ./clock_test
 */

#include "../core/Clock.hpp"
#include <cstdio>
#include <cmath>

int main() {
    constexpr double kBPM = 150.0;
    constexpr int kSampleRate = 48000;
    constexpr int kNumFrames = 4800000; // 100 秒
    constexpr int kSeconds = kNumFrames / kSampleRate;
    constexpr double kExpectedTicks = kBPM * kSeconds / 60.0; // 250

    Clock clock(kSampleRate);
    clock.SetBPM(kBPM);

    auto ticks = clock.Process(kNumFrames);
    const size_t actualTicks = ticks.size();

    double expectedInterval = 60.0 * kSampleRate / kBPM; // = 19200.0

    printf("BPM:              %.1f\n", kBPM);
    printf("SampleRate:       %d\n", kSampleRate);
    printf("Duration:         %d seconds (%d frames)\n", kSeconds, kNumFrames);
    printf("Expected ticks:   %.6f\n", kExpectedTicks);
    printf("Actual ticks:     %zu\n", actualTicks);
    printf("Expected interval: %.6f frames\n", expectedInterval);

    // 验证 tick 数量
    bool countOk = (actualTicks == static_cast<size_t>(kExpectedTicks));

    // 验证所有 interval 一致性
    bool intervalsConsistent = true;
    if (actualTicks > 1) {
        int refInterval = ticks[1] - ticks[0];
        for (size_t i = 2; i < actualTicks; ++i) {
            if (ticks[i] - ticks[i - 1] != refInterval) {
                intervalsConsistent = false;
                break;
            }
        }
    }

    printf("Tick count correct:  %s\n", countOk ? "yes" : "no");
    printf("Intervals consistent: %s\n", intervalsConsistent ? "yes" : "no");

    // 相位误差验证：检查第 N 个 tick 的理论帧位置 vs 实际帧位置
    // tick[t] (0-indexed) 的理论位置 = (t+1) * framesPerTick 步处理完毕，即帧索引 (t+1) * framesPerTick - 1
    double maxPhaseError = 0.0;
    printf("\n--- Tick position error (phase accuracy) ---\n");
    for (size_t t = 0; t < actualTicks; ++t) {
        double expectedPos = static_cast<double>(t + 1) * expectedInterval - 1.0;
        double actualPos = static_cast<double>(ticks[t]);
        double error = actualPos - expectedPos;
        if (std::abs(error) > maxPhaseError) maxPhaseError = std::abs(error);

        if (t < 5 || t >= actualTicks - 2) {
            printf("tick[%4zu]: frame %7d, expected %.4f, error %.4e\n",
                   t + 1, ticks[t], expectedPos, error);
        } else if (t == 5) {
            printf("  ...\n");
        }
    }

    // 计算每步的相位误差 = 最大帧误差 / interval = 帧误差 * deltaPhi
    // 相位范围 [0, 1.0)，每个 tick 对应 phase 过 1.0
    // 帧误差 framesError 对应相位误差 framesError * deltaPhi = framesError * BPM / (60 * sampleRate)
    double deltaPhi = kBPM / (60.0 * kSampleRate);
    double maxPhaseDrift = maxPhaseError * deltaPhi;

    printf("\n--- Phase error summary ---\n");
    printf("Max frame error:     %.6e frames\n", maxPhaseError);
    printf("Max phase drift:     %.6e  (Δφ=%.6e)\n", maxPhaseDrift, deltaPhi);

    // 连续运行 100 秒，相位漂移 < 1e-15
    bool phaseOk = (maxPhaseDrift < 1e-15);
    printf("Phase drift < 1e-15: %s\n", phaseOk ? "yes" : "no");

    bool pass = countOk && intervalsConsistent && phaseOk;
    printf("\n=== %s ===\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
