/**
 * CadenceDetector.hpp 验收测试
 *
 * 合成信号：重力 + 1.5Hz 摆臂 + 落脚冲击串 + 噪声，支持匀速、斜坡、漏步与
 * 误触发注入；默认 200Hz，可切 100Hz 并叠加时间戳抖动。
 *
 * 编译：g++ -std=c++17 -O2 -I../.. -o cadence_detector_test cadence_detector_test.cpp
 */

#include "../core/CadenceDetector.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace {

int g_failures = 0;

void Check(const char* name, bool ok, const char* detail = "") {
    std::printf("[%s] %s %s\n", ok ? "PASS" : "FAIL", name, detail);
    if (!ok) ++g_failures;
}

constexpr double kTwoPi = 6.28318530717958647692;
constexpr std::int64_t kSecondNs = 1000000000LL;

/** 跑步信号：可匀速或线性变速，可注入漏步与额外冲击。 */
struct Signal {
    double spmStart = 180.0;
    double spmEnd = -1.0;          // <0 表示不变速
    double rampStartSec = 0.0;
    double rampEndSec = 0.0;
    double impactAmp = 6.0;
    double swingAmp = 2.0;
    double noiseAmp = 0.15;
    double skipAtSec = -1.0;       // 该时刻所在的一拍不产生冲击
    double spuriousDelaySec = -1.0; // 相对步点的额外冲击时刻
    int spuriousEvery = 0;         // 每 N 拍注入一次
    std::mt19937 rng{2024};

    double SpmAt(double t) const {
        if (spmEnd <= 0.0) return spmStart;
        double u = (t - rampStartSec) / std::max(1e-6, rampEndSec - rampStartSec);
        u = std::clamp(u, 0.0, 1.0);
        return spmStart + (spmEnd - spmStart) * u;
    }

    double Sample(double t, double ageSec, bool isSpuriousCycle, bool skipImpact) {
        const double swing = swingAmp * std::sin(kTwoPi * 1.5 * t);
        const double drift = 0.5 * std::sin(kTwoPi * 0.5 * t);

        double impact = 0.0;
        if (!skipImpact && ageSec < 0.10) {
            impact += impactAmp * std::exp(-ageSec / 0.02) * std::sin(kTwoPi * 12.0 * ageSec);
        }
        if (spuriousDelaySec > 0.0 && isSpuriousCycle) {
            const double age = ageSec - spuriousDelaySec;
            if (age >= 0.0 && age < 0.10) {
                impact += impactAmp * 0.9 * std::exp(-age / 0.02) * std::sin(kTwoPi * 12.0 * age);
            }
        }
        std::uniform_real_distribution<double> noise(-noiseAmp, noiseAmp);
        return 9.81 + swing + drift + impact + noise(rng);
    }
};

struct Runner {
    cadence::CadenceDetector detector;
    std::mt19937 jitterRng{99};
    std::int64_t ts = kSecondNs;
    double sampleRate = 200.0;
    double t = 0.0;
    double age = 0.0;
    long cycle = 0;
    double cycleStart = 0.0;

    void Start(double targetSpm, double rateHz = 200.0) {
        detector.Reset();
        sampleRate = rateHz;
        detector.SetSampleRate(rateHz);
        detector.SetTarget(targetSpm);
        ts = kSecondNs;
        t = 0.0;
        age = 0.0;
        cycle = 0;
        cycleStart = 0.0;
    }

    void Feed(double seconds, Signal& signal, bool jitter = false) {
        const double dt = 1.0 / sampleRate;
        const int samples = static_cast<int>(seconds * sampleRate);
        std::uniform_real_distribution<double> jitterDist(-0.0025, 0.0025);
        for (int i = 0; i < samples; ++i) {
            const double spm = signal.SpmAt(t);
            const double period = 60.0 / spm;
            age += dt;
            if (age >= period) {
                age -= period;
                ++cycle;
                cycleStart = t;
            }
            const bool skip = signal.skipAtSec >= 0.0 &&
                              cycleStart >= signal.skipAtSec &&
                              cycleStart < signal.skipAtSec + period;
            const bool spurious = signal.spuriousEvery > 0 && cycle % signal.spuriousEvery == 0;

            const double magnitude = signal.Sample(t, age, spurious, skip);
            std::int64_t delta = static_cast<std::int64_t>(dt * kSecondNs);
            if (jitter) delta += static_cast<std::int64_t>(jitterDist(jitterRng) * kSecondNs);
            if (delta <= 0) delta = 1;
            ts += delta;
            t += dt;
            detector.Push(0.0f, 0.0f, static_cast<float>(magnitude), ts);
        }
    }
};

void TestSteadyCadence() {
    const double cases[] = {120.0, 150.0, 180.0, 210.0};
    for (double spm : cases) {
        Runner runner;
        Signal signal;
        signal.spmStart = spm;
        runner.Start(spm);
        runner.Feed(12.0, signal);

        const auto snapshot = runner.detector.GetSnapshot();
        char detail[128];
        std::snprintf(detail, sizeof(detail), "spm=%.0f cadence=%.1f steps=%lld env=%.2f",
                      spm, snapshot.cadenceSpm, snapshot.steps, snapshot.envelope);
        Check("steady cadence", snapshot.tracking && std::abs(snapshot.cadenceSpm - spm) <= 2.0, detail);
    }
}

void TestStationary() {
    Runner runner;
    Signal signal;
    signal.impactAmp = 0.0;
    signal.swingAmp = 0.6;
    signal.noiseAmp = 0.3;
    runner.Start(180.0);
    runner.Feed(10.0, signal);

    const auto snapshot = runner.detector.GetSnapshot();
    char detail[128];
    std::snprintf(detail, sizeof(detail), "cadence=%.1f steps=%lld env=%.2f",
                  snapshot.cadenceSpm, snapshot.steps, snapshot.envelope);
    Check("stationary rejected", !snapshot.tracking && snapshot.cadenceSpm == 0.0, detail);
}

void TestGapRecovery() {
    Runner runner;
    Signal running;
    running.spmStart = 180.0;
    runner.Start(180.0);
    runner.Feed(6.0, running);
    const auto before = runner.detector.GetSnapshot();

    Signal idle;
    idle.impactAmp = 0.0;
    idle.swingAmp = 0.4;
    idle.noiseAmp = 0.1;
    runner.Feed(3.0, idle);
    const auto during = runner.detector.GetSnapshot();

    runner.Feed(6.0, running);
    const auto after = runner.detector.GetSnapshot();

    bool statsPreserved = after.steps > before.steps && after.steps > 0;
    bool timeFrozen = during.activeSec - before.activeSec <= 1.6;
    bool recovered = after.tracking && std::abs(after.cadenceSpm - 180.0) <= 3.0;

    char detail[160];
    std::snprintf(detail, sizeof(detail), "before=%lld during=%.1f after=%lld cadence=%.1f",
                  before.steps, during.activeSec, after.steps, after.cadenceSpm);
    Check("gap keeps stats", statsPreserved && timeFrozen, detail);
    Check("gap recovers cadence", recovered, detail);
}

void TestReset() {
    Runner runner;
    Signal signal;
    runner.Start(180.0);
    runner.Feed(6.0, signal);
    runner.detector.Reset();

    const auto snapshot = runner.detector.GetSnapshot();
    float history[cadence::CadenceDetector::kHistorySize] = {};
    const int valid = runner.detector.CopyHistory(history, cadence::CadenceDetector::kHistorySize);
    Check("reset clears", !snapshot.tracking && snapshot.steps == 0 && snapshot.activeSec == 0.0 && valid == 0);
}

void TestTimestampJitter() {
    Runner runner;
    Signal signal;
    signal.spmStart = 180.0;
    runner.Start(180.0);
    runner.Feed(12.0, signal, /*jitter=*/true);

    const auto snapshot = runner.detector.GetSnapshot();
    char detail[128];
    std::snprintf(detail, sizeof(detail), "cadence=%.1f", snapshot.cadenceSpm);
    Check("jitter tolerated", snapshot.tracking && std::abs(snapshot.cadenceSpm - 180.0) <= 3.0, detail);
}

void TestTargetSwitch() {
    Runner runner;
    Signal signal;
    signal.spmStart = 180.0;
    runner.Start(180.0);
    runner.Feed(6.0, signal);
    const auto matched = runner.detector.GetSnapshot();

    runner.detector.SetTarget(150.0);
    runner.Feed(4.0, signal);
    const auto shifted = runner.detector.GetSnapshot();

    char detail[160];
    std::snprintf(detail, sizeof(detail), "matched=%.2f shifted=%.2f target=%.1f",
                  matched.accuracy, shifted.accuracy, shifted.targetSpm);
    Check("accuracy follows target", matched.accuracy > 0.9 && shifted.accuracy > 0.4 && shifted.accuracy < 0.8,
          detail);
}

void TestMissedStep() {
    Runner runner;
    Signal signal;
    signal.spmStart = 180.0;
    signal.skipAtSec = 6.0;
    runner.Start(180.0);
    runner.Feed(12.0, signal);

    const auto snapshot = runner.detector.GetSnapshot();
    const long long expected = static_cast<long long>(12.0 * 180.0 / 60.0);
    char detail[160];
    std::snprintf(detail, sizeof(detail), "cadence=%.1f steps=%lld expected~%lld",
                  snapshot.cadenceSpm, snapshot.steps, expected);
    Check("missed step ignored", snapshot.tracking && std::abs(snapshot.cadenceSpm - 180.0) <= 3.0, detail);
    Check("missed step counted", snapshot.steps >= expected - 2, detail);
}

void TestLowerSampleRate() {
    Runner runner;
    Signal signal;
    signal.spmStart = 180.0;
    runner.Start(180.0, 100.0);
    runner.Feed(12.0, signal);

    const auto snapshot = runner.detector.GetSnapshot();
    char detail[128];
    std::snprintf(detail, sizeof(detail), "cadence=%.1f", snapshot.cadenceSpm);
    Check("100Hz input", snapshot.tracking && std::abs(snapshot.cadenceSpm - 180.0) <= 3.0, detail);
}

void TestHistory() {
    Runner runner;
    Signal signal;
    signal.spmStart = 180.0;
    runner.Start(180.0);
    runner.Feed(10.0, signal);

    float history[cadence::CadenceDetector::kHistorySize] = {};
    const int valid = runner.detector.CopyHistory(history, cadence::CadenceDetector::kHistorySize);
    bool newestValid = history[cadence::CadenceDetector::kHistorySize - 1] > 150.0f;
    char detail[128];
    std::snprintf(detail, sizeof(detail), "valid=%d newest=%.1f", valid, history[59]);
    Check("history filled", valid >= 9 && newestValid, detail);
}

void TestOutOfOrderIgnored() {
    Runner runner;
    Signal signal;
    signal.spmStart = 180.0;
    runner.Start(180.0);
    runner.Feed(6.0, signal);
    const auto before = runner.detector.GetSnapshot();
    runner.detector.Push(0.0f, 0.0f, 9.81f, 1);
    const auto after = runner.detector.GetSnapshot();
    Check("out of order ignored", before.steps == after.steps && before.cadenceSpm == after.cadenceSpm);
}

/** 冲刺：3s 内 180→240，显示必须单调上升且不超调。 */
void TestSprintAcceleration() {
    Runner runner;
    Signal signal;
    signal.spmStart = 180.0;
    signal.spmEnd = 240.0;
    signal.rampStartSec = 2.0;
    signal.rampEndSec = 5.0;
    runner.Start(200.0);

    std::vector<double> samples;
    for (int i = 0; i < 24; ++i) {   // 12s，每 0.5s 采样
        runner.Feed(0.5, signal);
        samples.push_back(runner.detector.GetSnapshot().cadenceSpm);
    }

    double maxUp = 0.0;
    double peak = 0.0;
    for (size_t i = 1; i < samples.size(); ++i) {
        if (samples[i] > 0 && samples[i - 1] > 0) {
            maxUp = std::max(maxUp, samples[i] - samples[i - 1]);
        }
        peak = std::max(peak, samples[i]);
    }
    const double finalValue = samples.back();
    char detail[160];
    std::snprintf(detail, sizeof(detail), "final=%.1f peak=%.1f maxUp=%.1f/0.5s", finalValue, peak, maxUp);
    Check("sprint follows", std::abs(finalValue - 240.0) <= 4.0, detail);
    Check("sprint bounded", peak <= 245.0 && maxUp <= 12.0, detail);
}

/** 170 SPM 下每 3 拍注入一次 250ms 后的误触发。 */
void TestSpuriousArtifacts() {
    Runner runner;
    Signal signal;
    signal.spmStart = 170.0;
    signal.spuriousDelaySec = 0.25;
    signal.spuriousEvery = 3;
    runner.Start(170.0);

    double peak = 0.0;
    for (int i = 0; i < 30; ++i) {
        runner.Feed(0.5, signal);
        peak = std::max(peak, runner.detector.GetSnapshot().cadenceSpm);
    }
    const double finalValue = runner.detector.GetSnapshot().cadenceSpm;
    char detail[160];
    std::snprintf(detail, sizeof(detail), "final=%.1f peak=%.1f", finalValue, peak);
    Check("artifact rejected", std::abs(finalValue - 170.0) <= 6.0 && peak <= 190.0, detail);
}

void TestWalkCadence() {
    Runner runner;
    Signal signal;
    signal.spmStart = 110.0;
    signal.swingAmp = 1.5;
    runner.Start(120.0);
    runner.Feed(12.0, signal);

    const auto snapshot = runner.detector.GetSnapshot();
    char detail[128];
    std::snprintf(detail, sizeof(detail), "cadence=%.1f", snapshot.cadenceSpm);
    Check("walk cadence", snapshot.tracking && std::abs(snapshot.cadenceSpm - 110.0) <= 4.0, detail);
}

void TestStopTimeout() {
    Runner runner;
    Signal running;
    running.spmStart = 180.0;
    runner.Start(180.0);
    runner.Feed(6.0, running);

    Signal idle;
    idle.impactAmp = 0.0;
    idle.swingAmp = 0.3;
    idle.noiseAmp = 0.08;
    runner.Feed(3.0, idle);

    const auto snapshot = runner.detector.GetSnapshot();
    char detail[128];
    std::snprintf(detail, sizeof(detail), "tracking=%d cadence=%.1f", snapshot.tracking ? 1 : 0, snapshot.cadenceSpm);
    Check("stop clears display", !snapshot.tracking && snapshot.cadenceSpm == 0.0, detail);
}

void TestCadenceRamp() {
    Runner runner;
    Signal signal;
    signal.spmStart = 150.0;
    signal.spmEnd = 190.0;
    signal.rampStartSec = 2.0;
    signal.rampEndSec = 10.0;
    runner.Start(170.0);
    runner.Feed(14.0, signal);

    const auto snapshot = runner.detector.GetSnapshot();
    char detail[128];
    std::snprintf(detail, sizeof(detail), "cadence=%.1f expected=190", snapshot.cadenceSpm);
    Check("ramp follows", snapshot.tracking && std::abs(snapshot.cadenceSpm - 190.0) <= 4.0, detail);
}

} // namespace

int main() {
    TestSteadyCadence();
    TestStationary();
    TestGapRecovery();
    TestReset();
    TestTimestampJitter();
    TestTargetSwitch();
    TestMissedStep();
    TestLowerSampleRate();
    TestHistory();
    TestOutOfOrderIgnored();
    TestSprintAcceleration();
    TestSpuriousArtifacts();
    TestWalkCadence();
    TestStopTimeout();
    TestCadenceRamp();

    if (g_failures == 0) {
        std::printf("\nALL PASS\n");
        return 0;
    }
    std::printf("\n%d FAILURE(S)\n", g_failures);
    return 1;
}
