#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>

/**
 * 步频检测 — 幅值 → 4 阶带通 → 快包络 → 动态阈值迟滞 → 间隔中位数 → SPM。
 * 输入为原始三轴加速度（m/s²）与单调时间戳（ns）。
 * 线程模型：传感器线程 Push，UI 线程读取快照；全部状态由内部互斥量保护，
 * 不进入音频回调线程。
 */
namespace cadence {

struct Snapshot {
    double cadenceSpm = 0.0;
    double avgSpm = 0.0;
    double maxSpm = 0.0;
    double accuracy = 0.0;      // 0..1
    double activeSec = 0.0;
    double targetSpm = 180.0;
    long long steps = 0;
    bool tracking = false;
    double envelope = 0.0;      // 当前包络，仅供真机标定日志
};

class CadenceDetector {
public:
    static constexpr int kHistorySize = 60;

    CadenceDetector() noexcept { ConfigureFilter(); }

    void Reset() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        ResetLocked();
    }

    void SetTarget(double spm) noexcept {
        if (!std::isfinite(spm)) return;
        std::lock_guard<std::mutex> lock(mutex_);
        targetSpm_ = std::clamp(spm, 0.0, 300.0);
        published_.targetSpm = targetSpm_;
    }

    void SetSampleRate(double hz) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!(hz >= 20.0 && hz <= 2000.0)) return;
        if (std::abs(hz - sampleRate_) < 1e-6) return;
        sampleRate_ = hz;
        ConfigureFilter();
    }

    void Push(float x, float y, float z, std::int64_t tsNanos) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        PushLocked(x, y, z, tsNanos);
    }

    [[nodiscard]] Snapshot GetSnapshot() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return published_;
    }

    /** 写入 capacity 个点（旧→新，不足处前补 0），返回有效点数。 */
    int CopyHistory(float* out, int capacity) const noexcept {
        if (out == nullptr || capacity <= 0) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        for (int i = 0; i < capacity; ++i) out[i] = 0.0f;
        const int available = static_cast<int>(historyCount_);
        const int n = std::min(capacity, available);
        for (int i = 0; i < n; ++i) {
            int index = historyHead_ - available + (available - n) + i;
            while (index < 0) index += kHistorySize;
            index %= kHistorySize;
            out[capacity - n + i] = history_[index];
        }
        return available;
    }

private:
    static constexpr double kPi = 3.14159265358979323846;
    static constexpr double kBandCenterHz = 11.2;
    static constexpr double kBandQ = 0.56;
    static constexpr double kEnvelopeTauSec = 0.10;
    static constexpr double kRiseRatio = 0.55;
    static constexpr double kRearmRatio = 0.25;
    static constexpr double kMinImpactMps2 = 0.6;   // 绝对门限：静止/噪声不产生步点
    static constexpr double kRearmFloorMps2 = 0.2;
    static constexpr double kBlankSec = 0.10;       // 步点后暂停重新武装，避免冲击内部次级波峰重复触发
    static constexpr double kRefractorySec = 0.25;
    static constexpr double kMinIntervalSec = 0.25;
    static constexpr double kMaxIntervalSec = 1.50;
    static constexpr double kWindowIdleSec = 8.0;
    static constexpr double kDisplayTimeoutSec = 2.5;
    static constexpr int kMedianWindow = 5;
    static constexpr double kMaxDeviationRatio = 0.12;
    static constexpr int kMaxOutliersBeforeReset = 4;
    static constexpr double kSmoothingAlpha = 0.35;
    static constexpr double kMaxSlewUpSpmPerSec = 20.0;
    static constexpr double kMaxSlewDownSpmPerSec = 40.0;
    static constexpr double kMinDisplaySpm = 90.0;
    static constexpr double kMaxDisplaySpm = 260.0;
    static constexpr int kMinSteps = 4;
    static constexpr double kSettleSec = 2.0;
    static constexpr double kAccuracyBandSpm = 3.0;
    static constexpr std::int64_t kNanosPerSec = 1000000000LL;

    struct Biquad {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double z1 = 0.0, z2 = 0.0;

        void ConfigureBandPass(double sampleRate, double centerHz, double q) noexcept {
            if (sampleRate <= 0.0 || centerHz <= 0.0 || centerHz >= sampleRate * 0.45 || q <= 0.0) {
                b0 = 1.0;
                b1 = b2 = a1 = a2 = 0.0;
                return;
            }
            const double w0 = 2.0 * kPi * centerHz / sampleRate;
            const double cosW = std::cos(w0);
            const double alpha = std::sin(w0) / (2.0 * q);
            const double a0 = 1.0 + alpha;
            b0 = alpha / a0;
            b1 = 0.0;
            b2 = -alpha / a0;
            a1 = -2.0 * cosW / a0;
            a2 = (1.0 - alpha) / a0;
        }

        double Process(double x) noexcept {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }

        void Reset() noexcept { z1 = 0.0; z2 = 0.0; }
    };

    void ConfigureFilter() noexcept {
        filterStage1_.ConfigureBandPass(sampleRate_, kBandCenterHz, kBandQ);
        filterStage2_.ConfigureBandPass(sampleRate_, kBandCenterHz, kBandQ);
        filterStage1_.Reset();
        filterStage2_.Reset();
        rawEnvelope_ = 0.0;
    }

    void ResetLocked() noexcept {
        ConfigureFilter();
        prevAbs_ = 0.0;
        prevThreshold_ = 0.0;
        armed_ = true;
        blankUntilNs_ = 0;
        prevTsNs_ = 0;
        lastPushNs_ = 0;
        lastStepNs_ = 0;
        lastHistoryNs_ = 0;
        trackingStartNs_ = 0;
        steps_ = 0;
        statSteps_ = 0;
        intervalCount_ = 0;
        consecutiveOutliers_ = 0;
        smoothedSpm_ = 0.0;
        displaySpm_ = 0.0;
        hasDisplay_ = false;
        hasCadence_ = false;
        tracking_ = false;
        activeSec_ = 0.0;
        accuracySec_ = 0.0;
        cadenceIntegral_ = 0.0;
        inTargetSec_ = 0.0;
        maxSpm_ = 0.0;
        historyHead_ = 0;
        historyCount_ = 0;
        history_.fill(0.0f);
        published_ = Snapshot{};
        published_.targetSpm = targetSpm_;
    }

    void PushLocked(float x, float y, float z, std::int64_t tsNanos) noexcept {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || tsNanos <= 0) return;
        if (lastPushNs_ == 0) {
            lastPushNs_ = tsNanos;
            prevTsNs_ = tsNanos;
            lastHistoryNs_ = tsNanos;
            return;
        }
        if (tsNanos <= lastPushNs_) return;

        const double dt = static_cast<double>(tsNanos - lastPushNs_) * 1e-9;
        lastPushNs_ = tsNanos;

        if (tracking_ && lastStepNs_ > 0 &&
            static_cast<double>(tsNanos - lastStepNs_) <= kMaxIntervalSec * kNanosPerSec) {
            activeSec_ += dt;
            cadenceIntegral_ += smoothedSpm_ * dt;
            // 起步 2s 内估计器仍在收敛，不计入跟拍率（分子分母同时排除）
            if (static_cast<double>(tsNanos - trackingStartNs_) > kSettleSec * kNanosPerSec) {
                accuracySec_ += dt;
                if (std::abs(displaySpm_ - targetSpm_) <= kAccuracyBandSpm) inTargetSec_ += dt;
            }
        }

        if (intervalCount_ > 0 && lastStepNs_ > 0 &&
            static_cast<double>(tsNanos - lastStepNs_) > kWindowIdleSec * kNanosPerSec) {
            intervalCount_ = 0;
            consecutiveOutliers_ = 0;
            hasCadence_ = false;
            smoothedSpm_ = 0.0;
        }

        const double magnitude = std::sqrt(static_cast<double>(x) * x +
                                           static_cast<double>(y) * y +
                                           static_cast<double>(z) * z);
        const double filtered = filterStage2_.Process(filterStage1_.Process(magnitude));
        const double rectified = std::abs(filtered);

        const double alpha = 1.0 - std::exp(-dt / kEnvelopeTauSec);
        rawEnvelope_ += alpha * (rectified - rawEnvelope_);
        const double threshold = std::max(kRiseRatio * rawEnvelope_, kMinImpactMps2);
        const double rearm = std::max(kRearmRatio * rawEnvelope_, kRearmFloorMps2);

        bool stepped = false;
        if (armed_ && prevAbs_ < prevThreshold_ && rectified >= threshold) {
            const double span = rectified - prevAbs_;
            double fraction = span > 1e-9 ? (threshold - prevAbs_) / span : 1.0;
            fraction = std::clamp(fraction, 0.0, 1.0);
            const auto stepNs = prevTsNs_ +
                                static_cast<std::int64_t>(fraction * static_cast<double>(tsNanos - prevTsNs_));
            stepped = OnStep(stepNs);
            armed_ = false;
            blankUntilNs_ = stepNs + static_cast<std::int64_t>(kBlankSec * kNanosPerSec);
        } else if (tsNanos >= blankUntilNs_ && rectified <= rearm) {
            armed_ = true;
        }

        prevAbs_ = rectified;
        prevThreshold_ = threshold;
        prevTsNs_ = tsNanos;

        UpdateTracking(tsNanos);
        if (tracking_) {
            if (!hasDisplay_) {
                displaySpm_ = std::clamp(smoothedSpm_, kMinDisplaySpm, kMaxDisplaySpm);
                hasDisplay_ = true;
            } else {
                const double rate = smoothedSpm_ > displaySpm_
                        ? kMaxSlewUpSpmPerSec : kMaxSlewDownSpmPerSec;
                const double diff = std::clamp(smoothedSpm_ - displaySpm_, -rate * dt, rate * dt);
                displaySpm_ = std::clamp(displaySpm_ + diff, kMinDisplaySpm, kMaxDisplaySpm);
            }
        }
        if (stepped && tracking_) ++statSteps_;
        UpdateHistory(tsNanos);
        Publish();
    }

    bool OnStep(std::int64_t stepNs) noexcept {
        if (lastStepNs_ == 0) {
            lastStepNs_ = stepNs;
            steps_ = 1;
            return true;
        }
        const double dt = static_cast<double>(stepNs - lastStepNs_) * 1e-9;
        if (dt < kRefractorySec) return false;

        ++steps_;
        if (dt >= kMinIntervalSec && dt <= kMaxIntervalSec) {
            AddInterval(dt);
        }
        lastStepNs_ = stepNs;
        return true;
    }

    void AddInterval(double dt) noexcept {
        if (hasCadence_ && smoothedSpm_ > 1.0) {
            const double candidate = 60.0 / dt;
            if (std::abs(candidate - smoothedSpm_) > kMaxDeviationRatio * smoothedSpm_) {
                if (++consecutiveOutliers_ >= kMaxOutliersBeforeReset) {
                    // 连续拒绝视为真的换挡：以最后一个间隔重新起步，显示值由变化率限制平滑跟随
                    intervalCount_ = 0;
                    intervals_[intervalCount_++] = dt;
                    smoothedSpm_ = candidate;
                    consecutiveOutliers_ = 0;
                }
                return;
            }
        }
        consecutiveOutliers_ = 0;

        if (intervalCount_ == kMedianWindow) {
            for (int i = 1; i < kMedianWindow; ++i) intervals_[i - 1] = intervals_[i];
            intervals_[kMedianWindow - 1] = dt;
        } else {
            intervals_[intervalCount_++] = dt;
        }

        const double raw = 60.0 / MedianInterval();
        smoothedSpm_ = hasCadence_ ? smoothedSpm_ + kSmoothingAlpha * (raw - smoothedSpm_) : raw;
        hasCadence_ = true;
    }

    [[nodiscard]] double MedianInterval() const noexcept {
        if (intervalCount_ == 0) return 0.0;
        std::array<double, kMedianWindow> sorted{};
        const int n = intervalCount_;
        for (int i = 0; i < n; ++i) sorted[i] = intervals_[i];
        for (int i = 1; i < n; ++i) {
            const double value = sorted[i];
            int j = i - 1;
            while (j >= 0 && sorted[j] > value) {
                sorted[j + 1] = sorted[j];
                --j;
            }
            sorted[j + 1] = value;
        }
        return (n % 2 == 1) ? sorted[n / 2] : 0.5 * (sorted[n / 2 - 1] + sorted[n / 2]);
    }

    void UpdateTracking(std::int64_t nowNs) noexcept {
        const bool wasTracking = tracking_;
        tracking_ = hasCadence_ && intervalCount_ > 0 && steps_ >= kMinSteps && lastStepNs_ > 0 &&
                    static_cast<double>(nowNs - lastStepNs_) <= kDisplayTimeoutSec * kNanosPerSec;
        if (tracking_ && !wasTracking) trackingStartNs_ = nowNs;
        if (tracking_ && static_cast<double>(nowNs - trackingStartNs_) > kSettleSec * kNanosPerSec) {
            maxSpm_ = std::max(maxSpm_, smoothedSpm_);
        }
    }

    void UpdateHistory(std::int64_t nowNs) noexcept {
        if (lastHistoryNs_ == 0) lastHistoryNs_ = nowNs;
        while (nowNs - lastHistoryNs_ >= kNanosPerSec) {
            history_[historyHead_] = tracking_ ? static_cast<float>(smoothedSpm_) : 0.0f;
            historyHead_ = (historyHead_ + 1) % kHistorySize;
            if (historyCount_ < kHistorySize) ++historyCount_;
            lastHistoryNs_ += kNanosPerSec;
        }
    }

    void Publish() noexcept {
        published_.cadenceSpm = tracking_ ? displaySpm_ : 0.0;
        published_.avgSpm = (activeSec_ >= 0.5 && cadenceIntegral_ > 0.0)
                                    ? cadenceIntegral_ / activeSec_
                                    : 0.0;
        published_.maxSpm = maxSpm_;
        published_.accuracy = accuracySec_ >= 0.5 ? std::clamp(inTargetSec_ / accuracySec_, 0.0, 1.0) : 0.0;
        published_.activeSec = activeSec_;
        published_.targetSpm = targetSpm_;
        published_.steps = steps_;
        published_.tracking = tracking_;
        published_.envelope = rawEnvelope_;
    }

    double targetSpm_ = 180.0;
    double sampleRate_ = 200.0;
    Biquad filterStage1_{};
    Biquad filterStage2_{};

    double rawEnvelope_ = 0.0;
    double prevAbs_ = 0.0;
    double prevThreshold_ = 0.0;
    bool armed_ = true;
    std::int64_t blankUntilNs_ = 0;
    std::int64_t prevTsNs_ = 0;
    std::int64_t lastPushNs_ = 0;

    std::int64_t lastStepNs_ = 0;
    std::int64_t lastHistoryNs_ = 0;
    std::int64_t trackingStartNs_ = 0;
    long long steps_ = 0;
    long long statSteps_ = 0;

    std::array<double, kMedianWindow> intervals_{};
    int intervalCount_ = 0;
    int consecutiveOutliers_ = 0;

    double smoothedSpm_ = 0.0;
    double displaySpm_ = 0.0;
    bool hasDisplay_ = false;
    bool hasCadence_ = false;
    bool tracking_ = false;

    double activeSec_ = 0.0;
    double accuracySec_ = 0.0;
    double cadenceIntegral_ = 0.0;
    double inTargetSec_ = 0.0;
    double maxSpm_ = 0.0;

    std::array<float, kHistorySize> history_{};
    int historyHead_ = 0;
    int historyCount_ = 0;

    Snapshot published_{};
    mutable std::mutex mutex_;
};

} // namespace cadence
