#include <jni.h>
#include <android/log.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <aaudio/AAudio.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/AudioEngine.hpp"
#include "core/Constants.hpp"
#include "core/WavLoader.hpp"

#define LOG_TAG "RunBeat-Native"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ========== 全局状态 ==========

static AudioEngine gEngine;
static AAudioStream* gStream = nullptr;
static int32_t gStreamSampleRate = beat::kDefaultSampleRate;
static int32_t gStreamChannels = 1;
static std::atomic<bool> gNativeInitialized{false};

// 已加载样本的重采样追踪（采样率变化后按新率重载）
// These vectors hold the samples AS RESAMPLED at gSamplesLoadedRate. Both the
// asset-loading path and the pure-fallback path publish here, so a later stream
// rebuild that negotiated a different hardware rate can re-resample (assets) or
// regenerate (fallback) at the new rate before pushing them back into gEngine.
static std::vector<float> gTickHiSamples;
static std::vector<float> gTickLoSamples;
static std::vector<float> gChimeSamples;
static bool gHaveChimeSamples = false;
static int gSamplesLoadedRate = 0;
static bool gUseAssetSamples = false;  // false = only generated fallback ticks

// 流断开标志：errorCallback（音频线程）写入，主线程检查并重建
static std::atomic<bool> gStreamError{false};

// WAV 加载失败时的 fallback：生成强/弱拍默认木鱼采样。
// sampleRate 必须与流实际采样率一致，否则 click 音高/时长失真。
static void GenerateDefaultTickSamples(std::vector<float>& outTickHi,
                                        std::vector<float>& outTickLo,
                                        int sampleRate) {
    outTickHi = WavGenerator::MakeWoodTick(sampleRate, true);
    outTickLo = WavGenerator::MakeWoodTick(sampleRate, false);
}

// ========== WAV 资源加载 ==========

/** 从 Android Assets 加载 WAV 文件 */
static bool LoadWavFromAsset(AAssetManager* mgr, const char* path,
                              std::vector<float>& outSamples, int targetRate) {
    AAsset* asset = AAssetManager_open(mgr, path, AASSET_MODE_BUFFER);
    if (!asset) {
        LOGE("Failed to open asset: %s", path);
        return false;
    }

    const uint8_t* data = static_cast<const uint8_t*>(AAsset_getBuffer(asset));
    off_t size = AAsset_getLength(asset);

    WavLoader loader;
    bool ok = loader.Load(data, static_cast<size_t>(size), targetRate);
    if (ok) {
        outSamples = loader.GetSamples();
        LOGI("Loaded %s: %zu samples (%d Hz)", path, outSamples.size(), targetRate);
    } else {
        LOGE("Failed to parse WAV: %s", path);
    }

    AAsset_close(asset);
    return ok;
}

/**
 * 重采样目标采样率：流打开后为流实际采样率，否则用默认值。
 */
static int CurrentTargetSampleRate() {
    return (gStreamSampleRate > 0) ? static_cast<int>(gStreamSampleRate)
                                   : beat::kDefaultSampleRate;
}

/**
 * 统一的样本装载入口 — nativeLoadWavAssets / nativeLoadSoundPack 的公共实现。
 *
 * 语义：
 *   - tickHi 必须可加载，否则不做任何装载（引擎保留现有样本）；
 *   - tickLo 为 null / 空串 / 加载失败 → 复用强拍样本（单音色），loOk=false；
 *   - chime 为 null / 空串 → 跳过（chimeOk=true，保留引擎中现有 chime）；
 *   - 所有样本按 CurrentTargetSampleRate() 重采样，并更新 gSamplesLoadedRate
 *     供采样率变化后的重载使用。
 *
 * @return 各部分独立的成功标志，布尔契约由调用方决定：
 *   nativeLoadWavAssets 要求 hi && lo && (提供的)chime 全部成功；
 *   nativeLoadSoundPack 只要求 hi 成功（lo 允许回退复用）。
 */
struct SampleLoadResult {
    bool hiOk;
    bool loOk;
    bool chimeOk;
};

static SampleLoadResult LoadSamplesIntoEngine(AAssetManager* mgr, const char* tickHiPath,
                                              const char* tickLoPath, const char* chimePath) {
    SampleLoadResult result{false, false, false};
    if (!mgr || tickHiPath == nullptr || tickHiPath[0] == '\0') return result;

    const int targetRate = CurrentTargetSampleRate();
    std::vector<float> hiSamples;
    result.hiOk = LoadWavFromAsset(mgr, tickHiPath, hiSamples, targetRate)
                  && !hiSamples.empty();
    if (!result.hiOk) {
        LOGE("LoadSamplesIntoEngine: failed to load tickHi");
        return result;
    }

    if (tickLoPath != nullptr && tickLoPath[0] != '\0') {
        std::vector<float> loSamples;
        result.loOk = LoadWavFromAsset(mgr, tickLoPath, loSamples, targetRate)
                      && !loSamples.empty();
        if (result.loOk) {
            gEngine.LoadSoundPack(hiSamples.data(), hiSamples.size(),
                                  loSamples.data(), loSamples.size());
            gTickHiSamples = hiSamples;
            gTickLoSamples = loSamples;
        }
    }
    if (!result.loOk) {
        // lo 缺失/失败：弱拍复用强拍样本（单音色）
        gEngine.LoadSoundPack(hiSamples.data(), hiSamples.size(),
                              hiSamples.data(), hiSamples.size());
        gTickHiSamples = hiSamples;
        gTickLoSamples = hiSamples;
    }
    gSamplesLoadedRate = targetRate;
    gUseAssetSamples = true;

    if (chimePath != nullptr && chimePath[0] != '\0') {
        std::vector<float> chimeSamples;
        result.chimeOk = LoadWavFromAsset(mgr, chimePath, chimeSamples, targetRate);
        if (result.chimeOk && !chimeSamples.empty()) {
            gEngine.LoadChimeSamples(chimeSamples.data(), chimeSamples.size());
            gChimeSamples = chimeSamples;
            gHaveChimeSamples = true;
        }
    } else {
        result.chimeOk = true;
    }

    LOGI("Samples loaded @%d Hz: hi=%s lo=%s chime=%s",
         targetRate, result.hiOk ? "ok" : "FAIL",
         result.loOk ? "ok" : "reuse-hi",
         (chimePath && chimePath[0]) ? (result.chimeOk ? "ok" : "FAIL") : "skipped");
    return result;
}

/**
 * 采样率变化后的样本重载。
 * Clock::SetSampleRate 已保证 BPM 节拍速度正确；这里只修正按旧率
 * 重采样的音色样本（音高/音色时长），不触碰时钟数学。
 */
static void ReloadSamplesIfSampleRateChanged() {
    if (gSamplesLoadedRate <= 0 || gSamplesLoadedRate == gStreamSampleRate) return;

    if (gUseAssetSamples) {
        gTickHiSamples = WavLoader::Resample(gTickHiSamples, gSamplesLoadedRate,
                                             gStreamSampleRate);
        gTickLoSamples = WavLoader::Resample(gTickLoSamples, gSamplesLoadedRate,
                                             gStreamSampleRate);
        gEngine.LoadSoundPack(gTickHiSamples.data(), gTickHiSamples.size(),
                              gTickLoSamples.data(), gTickLoSamples.size());
        if (gHaveChimeSamples) {
            gChimeSamples = WavLoader::Resample(gChimeSamples, gSamplesLoadedRate,
                                                gStreamSampleRate);
            gEngine.LoadChimeSamples(gChimeSamples.data(), gChimeSamples.size());
        }
    } else {
        std::vector<float> tickHi = WavGenerator::MakeWoodTick(gStreamSampleRate, true);
        std::vector<float> tickLo = WavGenerator::MakeWoodTick(gStreamSampleRate, false);
        gEngine.LoadSoundPack(tickHi.data(), tickHi.size(),
                              tickLo.data(), tickLo.size());
        gTickHiSamples = std::move(tickHi);
        gTickLoSamples = std::move(tickLo);
    }
    LOGI("Samples reloaded for sample-rate change: %d → %d Hz",
         gSamplesLoadedRate, gStreamSampleRate);
    gSamplesLoadedRate = gStreamSampleRate;
}

// ========== AAudio 回调 ==========

// 复用的 mono 临时缓冲：音频线程独占，nativeInit 中预 reserve 4096 帧；
// callback 只在容量足够时 resize，否则输出静音 — 不在回调中扩容。
static std::vector<float> gMonoBuf;

static aaudio_data_callback_result_t dataCallback(
        AAudioStream* /*stream*/, void* /*userData*/, void* audioData, int32_t numFrames) {
    float* out = static_cast<float*>(audioData);

    if (gStreamChannels <= 1) {
        gEngine.OnAudioCallback(out, numFrames);
    } else {
        // Defensive path only：OpenStream 固定请求 channelCount=1，
        // 当前配置下 gStreamChannels > 1 不会被选择；保留以防未来放开单声道限制。
        if (gMonoBuf.capacity() < static_cast<size_t>(numFrames)) {
            // 回调线程禁止扩容；异常大 buffer 直接输出静音，等待上层恢复。
            const size_t outputSamples = static_cast<size_t>(numFrames) *
                    static_cast<size_t>(gStreamChannels);
            std::fill_n(out, outputSamples, 0.0f);
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        }
        // 多通道（一般为 stereo）：先生成 mono，再交错写入每个通道。
        gMonoBuf.resize(static_cast<size_t>(numFrames));
        gEngine.OnAudioCallback(gMonoBuf.data(), numFrames);
        const int ch = gStreamChannels;
        for (int i = 0; i < numFrames; ++i) {
            float s = gMonoBuf[i];
            for (int c = 0; c < ch; ++c) {
                out[i * ch + c] = s;
            }
        }
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void errorCallback(AAudioStream* /*stream*/, void* /*userData*/, aaudio_result_t error) {
    LOGI("stream error: %d", error);
    // 不可在 error callback 内调用任何 AAudio API，只设标志位由主线程轮询重建
    if (error == AAUDIO_ERROR_DISCONNECTED) {
        gStreamError.store(true, std::memory_order_relaxed);
        LOGI("stream disconnected — flagged for rebuild");
    }
}

// ========== 流管理 ==========

static bool OpenStream() {
    if (gStream != nullptr) return true;

    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK) {
        LOGE("AAudio_createStreamBuilder failed: %d", result);
        return false;
    }

    // 核心常量集中于 core/Constants.hpp
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    // 固定 mono + 48kHz：多通道后半 buffer 会读到未初始化内存，且避免 AAudio 重采样漂移
    AAudioStreamBuilder_setChannelCount(builder, 1);
    AAudioStreamBuilder_setSampleRate(builder, beat::kDefaultSampleRate);
    AAudioStreamBuilder_setDataCallback(builder, dataCallback, nullptr);
    AAudioStreamBuilder_setErrorCallback(builder, errorCallback, nullptr);

    // 尝试独占模式 → 降级共享
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_EXCLUSIVE);
    result = AAudioStreamBuilder_openStream(builder, &gStream);
    if (result != AAUDIO_OK) {
        LOGI("EXCLUSIVE mode failed (%d), fallback to SHARED", result);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        result = AAudioStreamBuilder_openStream(builder, &gStream);
    }

    AAudioStreamBuilder_delete(builder);

    if (result != AAUDIO_OK) {
        LOGE("AAudioStreamBuilder_openStream failed: %d", result);
        gStream = nullptr;
        return false;
    }

    // 查询实际采样率/通道数并同步引擎
    gStreamSampleRate = AAudioStream_getSampleRate(gStream);
    gStreamChannels   = AAudioStream_getChannelCount(gStream);
    // 采样率变化后的样本重载：音高/音色时长漂移修正。
    // 节拍速度不受影响（Clock::SetSampleRate 已重算 framesPerTick），勿动时钟数学。
    ReloadSamplesIfSampleRateChanged();
    gEngine.SetSampleRate(gStreamSampleRate);

    // 让 AAudio 选择合适的 buffer 容量（默认通常 = 2 burst），降低 underrun
    int32_t framesPerBurst = AAudioStream_getFramesPerBurst(gStream);
    AAudioStream_setBufferSizeInFrames(gStream, framesPerBurst * 2);

    LOGI("stream opened: %d Hz, %d ch, %s mode, burst=%d",
         gStreamSampleRate, gStreamChannels,
         AAudioStream_getSharingMode(gStream) == AAUDIO_SHARING_MODE_EXCLUSIVE
             ? "EXCLUSIVE" : "SHARED",
         framesPerBurst);

    return true;
}

static void CloseStream() {
    if (gStream != nullptr) {
        AAudioStream_requestStop(gStream);
        AAudioStream_close(gStream);
        gStream = nullptr;
        LOGI("stream closed");
    }
}

/**
 * 重建 AAudio 流并按当前引擎状态决定是否立即启动。
 * 必须在非音频线程调用（主线程 / JNI 线程均可）。
 */
static bool RebuildStream() {
    LOGI("RebuildStream: closing dead stream");
    CloseStream();

    if (!OpenStream()) {
        LOGE("RebuildStream: OpenStream failed");
        return false;
    }

    // Running/Paused 均重启流（Paused 时引擎输出静音，Resume 零延迟）；Idle 只 Open
    AudioEngine::State s = gEngine.GetState();
    if (s == AudioEngine::State::Running || s == AudioEngine::State::Paused) {
        aaudio_result_t result = AAudioStream_requestStart(gStream);
        if (result == AAUDIO_OK) {
            LOGI("RebuildStream: stream restarted (state=%d)", static_cast<int>(s));
        } else {
            LOGE("RebuildStream: requestStart failed: %d", result);
            return false;
        }
    } else {
        LOGI("RebuildStream: engine idle, stream opened but not started");
    }
    return true;
}

/** 初始化流及 fallback 样本；供 Activity 与 sticky Service 的恢复路径共用。 */
static bool InitializeEngine() {
    if (!OpenStream()) return false;

    if (!gNativeInitialized.load(std::memory_order_acquire)) {
        std::vector<float> tickHi, tickLo;
        GenerateDefaultTickSamples(tickHi, tickLo, gStreamSampleRate);
        gEngine.LoadTickSamples(tickHi.data(), tickHi.size());
        gEngine.LoadTickLoSamples(tickLo.data(), tickLo.size());

        // 记录样本率，采样率变化后按新率重新生成（ReloadSamplesIfSampleRateChanged）
        gTickHiSamples = std::move(tickHi);
        gTickLoSamples = std::move(tickLo);
        gSamplesLoadedRate = gStreamSampleRate;
        gUseAssetSamples = false;

        if (gMonoBuf.capacity() < 4096) gMonoBuf.reserve(4096);
        gNativeInitialized.store(true, std::memory_order_release);
    }
    return true;
}

// ========== JNI — 引擎生命周期 ==========

extern "C" JNIEXPORT jboolean JNICALL
Java_com_runbeat_audio_AudioEngine_nativeInit(JNIEnv* /*env*/, jclass /*clazz*/) {
    LOGI("nativeInit");
    // 打开流但不启动；实际失败会反馈给 Java，之后可由 START 重试。
    return InitializeEngine() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_runbeat_audio_AudioEngine_nativeStart(JNIEnv * /*env*/, jclass /*clazz*/, jdouble bpm) {
    LOGI("nativeStart(%.1f)", bpm);
    if (gStreamError.exchange(false, std::memory_order_acq_rel)) {
        CloseStream();
    }
    if (!InitializeEngine() || gStream == nullptr) {
        LOGE("nativeStart: audio engine initialization failed");
        return JNI_FALSE;
    }
    gEngine.Start(static_cast<double>(bpm));
    // 启动 AAudio 流（OPEN/STOPPED/PAUSED → STARTED）
    aaudio_result_t r = AAudioStream_requestStart(gStream);
    if (r != AAUDIO_OK) {
        LOGE("nativeStart: requestStart failed: %d", r);
        gEngine.Stop();
        gStreamError.store(true, std::memory_order_release);
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativeStop(JNIEnv * /*env*/, jclass /*clazz*/) {
    LOGI("nativeStop");
    gEngine.Stop();
    if (gStream) {
        aaudio_result_t r = AAudioStream_requestStop(gStream);
        if (r != AAUDIO_OK) LOGE("nativeStop: requestStop failed: %d", r);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativePause(JNIEnv * /*env*/, jclass /*clazz*/) {
    LOGI("nativePause");
    gEngine.Pause();
    if (gStream) {
        aaudio_result_t r = AAudioStream_requestPause(gStream);
        if (r != AAUDIO_OK) LOGE("nativePause: requestPause failed: %d", r);
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_runbeat_audio_AudioEngine_nativeResume(JNIEnv * /*env*/, jclass /*clazz*/) {
    LOGI("nativeResume");
    if (gStreamError.exchange(false, std::memory_order_acq_rel)) {
        CloseStream();
    }
    if (gStream == nullptr && !InitializeEngine()) return JNI_FALSE;
    if (gStream == nullptr) return JNI_FALSE;
    gEngine.Resume();
    // 从 PAUSED/STOPPED 状态重新启动 AAudio 流
    aaudio_result_t r = AAudioStream_requestStart(gStream);
    if (r != AAUDIO_OK) {
        LOGE("nativeResume: requestStart failed: %d", r);
        gEngine.Pause();
        gStreamError.store(true, std::memory_order_release);
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_runbeat_audio_AudioEngine_nativeGetState(JNIEnv* /*env*/, jclass /*clazz*/) {
    return static_cast<jint>(gEngine.GetState());
}

// ========== JNI — 运行时参数 ==========
// 这些 setter 属高频调用（滑条拖动/BPM 连续触发），不逐次 LOGI 刷屏。

extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativeSetBpm(JNIEnv * /*env*/, jclass /*clazz*/, jdouble bpm) {
    gEngine.SetBpm(static_cast<double>(bpm));
}

extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativeSetTickVolume(JNIEnv * /*env*/, jclass /*clazz*/, jdouble vol) {
    gEngine.SetTickVolume(static_cast<double>(vol));
}

// PLANNED（整点报时）：Java 层暂未接线，接口保留。
extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativeSetChimeVolume(JNIEnv * /*env*/, jclass /*clazz*/, jdouble vol) {
    gEngine.SetChimeVolume(static_cast<double>(vol));
}

extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativeSetAccent(JNIEnv * /*env*/, jclass /*clazz*/, jboolean on) {
    gEngine.SetAccent(static_cast<bool>(on));
}

extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativeSetOutputGain(JNIEnv * /*env*/, jclass /*clazz*/, jdouble gain) {
    gEngine.SetOutputGain(static_cast<double>(gain));
}

/**
 * 从 Assets 加载 WAV 资源到引擎。
 * 共用 LoadSamplesIntoEngine；保留原 Java 签名
 * （AudioEngine.nativeLoadWavAssets(mgr, hi, lo, chime)）。
 * 契约：hi、lo、（提供的）chime 任一加载失败即返回 false（chime 路径缺失
 * 则不作为失败条件）。
 */
extern "C" JNIEXPORT jboolean JNICALL
Java_com_runbeat_audio_AudioEngine_nativeLoadWavAssets(
        JNIEnv* env, jclass /*clazz*/,
        jobject assetManagerObj,
        jstring tickHiPath, jstring tickLoPath, jstring chimePath) {
    AAssetManager* mgr = AAssetManager_fromJava(env, assetManagerObj);
    if (!mgr) return JNI_FALSE;

    const char* cHi  = (tickHiPath != nullptr) ? env->GetStringUTFChars(tickHiPath, nullptr) : nullptr;
    const char* cLo  = (tickLoPath != nullptr) ? env->GetStringUTFChars(tickLoPath, nullptr) : nullptr;
    const char* cChime = (chimePath != nullptr) ? env->GetStringUTFChars(chimePath, nullptr) : nullptr;

    const SampleLoadResult r = LoadSamplesIntoEngine(mgr, cHi, cLo, cChime);
    const bool ok = r.hiOk && r.loOk && r.chimeOk;

    if (cHi != nullptr) env->ReleaseStringUTFChars(tickHiPath, cHi);
    if (cLo != nullptr) env->ReleaseStringUTFChars(tickLoPath, cLo);
    if (cChime != nullptr) env->ReleaseStringUTFChars(chimePath, cChime);
    return ok ? JNI_TRUE : JNI_FALSE;
}

/**
 * 加载成对音色（强拍 + 弱拍）。
 * 共用 LoadSamplesIntoEngine；保留原 Java 签名。
 * 契约：仅要求 tickHi 成功；tickLo 缺失/失败时两路复用强拍样本（单音色）。
 */
extern "C" JNIEXPORT jboolean JNICALL
Java_com_runbeat_audio_AudioEngine_nativeLoadSoundPack(
        JNIEnv* env, jclass /*clazz*/,
        jobject assetManagerObj,
        jstring tickHiPath, jstring tickLoPath) {
    AAssetManager* mgr = AAssetManager_fromJava(env, assetManagerObj);
    if (!mgr) return JNI_FALSE;

    const char* cHi = (tickHiPath != nullptr) ? env->GetStringUTFChars(tickHiPath, nullptr) : nullptr;
    const char* cLo = (tickLoPath != nullptr) ? env->GetStringUTFChars(tickLoPath, nullptr) : nullptr;

    const SampleLoadResult r = LoadSamplesIntoEngine(mgr, cHi, cLo, nullptr);  // chime: 不在此入口触碰

    if (cHi != nullptr) env->ReleaseStringUTFChars(tickHiPath, cHi);
    if (cLo != nullptr) env->ReleaseStringUTFChars(tickLoPath, cLo);
    return r.hiOk ? JNI_TRUE : JNI_FALSE;
}

// PLANNED（整点报时）：Java 层暂未接线，接口保留。
extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativeTriggerChime(JNIEnv * /*env*/, jclass /*clazz*/) {
    gEngine.TriggerChime();
}

// ========== JNI — 流健康检查（由主线程定期调用） ==========

/**
 * 检查并重建断开的 AAudio 流。
 * 在 Java 层 1 秒定时器中调用，保证在主线程执行。
 * 返回 true 表示流被重建过。
 */
extern "C" JNIEXPORT jboolean JNICALL
Java_com_runbeat_audio_AudioEngine_nativeRebuildStreamIfNeeded(
        JNIEnv* /*env*/, jclass /*clazz*/) {
    if (!gStreamError.load(std::memory_order_relaxed)) {
        return JNI_FALSE;
    }
    gStreamError.store(false, std::memory_order_relaxed);
    LOGI("nativeRebuildStreamIfNeeded: rebuilding");
    const bool rebuilt = RebuildStream();
    if (!rebuilt) gStreamError.store(true, std::memory_order_release);
    return rebuilt ? JNI_TRUE : JNI_FALSE;
}

// ========== JNI — 清理 ==========

extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativeDestroy(JNIEnv* /*env*/, jclass /*clazz*/) {
    LOGI("nativeDestroy");
    gEngine.Stop();
    CloseStream();
    gNativeInitialized.store(false, std::memory_order_release);
}
