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
#include "core/WavLoader.hpp"

#define LOG_TAG "RunBeat-Native"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ========== 全局状态 ==========

static AudioEngine gEngine;
static AAudioStream* gStream = nullptr;
static int32_t gStreamSampleRate = 48000;
static int32_t gStreamChannels = 1;
static std::atomic<bool> gNativeInitialized{false};

// 流断开标志：errorCallback（音频线程）写入，主线程检查并重建
static std::atomic<bool> gStreamError{false};

// ========== 采样生成 ==========

/**
 * 生成默认木鱼采样（WAV 加载失败时的 fallback）。
 * 使用 WavGenerator::MakeWoodTick 同时生成强拍和弱拍。
 * @param sampleRate 必须与 AAudio 流实际采样率一致，否则 click 速率/音高会失真。
 */
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

/** 从 Assets 加载 WAV 资源到引擎 */
extern "C" JNIEXPORT jboolean JNICALL
Java_com_runbeat_audio_AudioEngine_nativeLoadWavAssets(
        JNIEnv* env, jclass /*clazz*/,
        jobject assetManagerObj,
        jstring tickHiPath, jstring tickLoPath, jstring chimePath) {
    AAssetManager* mgr = AAssetManager_fromJava(env, assetManagerObj);
    if (!mgr) return JNI_FALSE;

    // 用流实际采样率重采样，避免音高/速度漂移
    int targetRate = (gStreamSampleRate > 0) ? gStreamSampleRate : 48000;
    std::vector<float> samples;
    bool tickHiOk = false;
    bool tickLoOk = false;
    bool chimeOk = false;

    // tick_hi
    const char* cTickHi = env->GetStringUTFChars(tickHiPath, nullptr);
    if (cTickHi != nullptr) {
        tickHiOk = LoadWavFromAsset(mgr, cTickHi, samples, targetRate);
        env->ReleaseStringUTFChars(tickHiPath, cTickHi);
        if (tickHiOk) gEngine.LoadTickSamples(samples.data(), samples.size());
    }

    // tick_lo (accent)
    const char* cTickLo = env->GetStringUTFChars(tickLoPath, nullptr);
    if (cTickLo != nullptr) {
        tickLoOk = LoadWavFromAsset(mgr, cTickLo, samples, targetRate);
        if (tickLoOk) gEngine.LoadTickLoSamples(samples.data(), samples.size());
        env->ReleaseStringUTFChars(tickLoPath, cTickLo);
    }

    // chime
    const char* cChime = env->GetStringUTFChars(chimePath, nullptr);
    if (cChime != nullptr) {
        chimeOk = LoadWavFromAsset(mgr, cChime, samples, targetRate);
        if (chimeOk) gEngine.LoadChimeSamples(samples.data(), samples.size());
        env->ReleaseStringUTFChars(chimePath, cChime);
    }

    LOGI("WAV assets loaded: hi=%d lo=%d chime=%d", tickHiOk, tickLoOk, chimeOk);
    return (tickHiOk && tickLoOk && chimeOk) ? JNI_TRUE : JNI_FALSE;
}

// ========== AAudio 回调 ==========

// 复用的 mono 临时缓冲（音频线程独占，nativeInit 中预 reserve 4096 帧；
// callback 只在容量足够时 resize，否则输出静音，不在回调中扩容）。
static std::vector<float> gMonoBuf;

static aaudio_data_callback_result_t dataCallback(
        AAudioStream* /*stream*/, void* /*userData*/, void* audioData, int32_t numFrames) {
    float* out = static_cast<float*>(audioData);

    if (gStreamChannels <= 1) {
        gEngine.OnAudioCallback(out, numFrames);
    } else {
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

    // 基础配置
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    // 固定 mono + 48kHz：多通道后半 buffer 会读到未初始化内存，且避免 AAudio 重采样漂移
    AAudioStreamBuilder_setChannelCount(builder, 1);
    AAudioStreamBuilder_setSampleRate(builder, 48000);
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

extern "C" JNIEXPORT jstring JNICALL
Java_com_runbeat_audio_AudioEngine_nativeHello(JNIEnv *env, jclass /*clazz*/) {
    LOGI("native loaded");
    return env->NewStringUTF("hello from native runbeat engine");
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

extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativeSetBpm(JNIEnv * /*env*/, jclass /*clazz*/, jdouble bpm) {
    LOGI("nativeSetBpm(%.1f)", bpm);
    gEngine.SetBpm(static_cast<double>(bpm));
}

extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativeSetTickVolume(JNIEnv * /*env*/, jclass /*clazz*/, jdouble vol) {
    LOGI("nativeSetTickVolume(%.2f)", vol);
    gEngine.SetTickVolume(static_cast<double>(vol));
}

extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativeSetChimeVolume(JNIEnv * /*env*/, jclass /*clazz*/, jdouble vol) {
    LOGI("nativeSetChimeVolume(%.2f)", vol);
    gEngine.SetChimeVolume(static_cast<double>(vol));
}

extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativeSetAccent(JNIEnv * /*env*/, jclass /*clazz*/, jboolean on) {
    LOGI("nativeSetAccent(%d)", on);
    gEngine.SetAccent(static_cast<bool>(on));
}

extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativeSetOutputGain(JNIEnv * /*env*/, jclass /*clazz*/, jdouble gain) {
    LOGI("nativeSetOutputGain(%.2f)", gain);
    gEngine.SetOutputGain(static_cast<double>(gain));
}

/**
 * 加载成对音色（强拍 + 弱拍）。
 * 若 tickLoPath 为空（null/空串），则两路都使用 tickHiPath（单音色）。
 */
extern "C" JNIEXPORT jboolean JNICALL
Java_com_runbeat_audio_AudioEngine_nativeLoadSoundPack(
        JNIEnv* env, jclass /*clazz*/,
        jobject assetManagerObj,
        jstring tickHiPath, jstring tickLoPath) {
    AAssetManager* mgr = AAssetManager_fromJava(env, assetManagerObj);
    if (!mgr || tickHiPath == nullptr) return JNI_FALSE;

    const char* cHi = env->GetStringUTFChars(tickHiPath, nullptr);
    if (cHi == nullptr) return JNI_FALSE;
    LOGI("nativeLoadSoundPack: hi=%s", cHi);

    // 用流实际采样率，避免非 48k 设备（蓝牙）音高漂移
    int targetRate = (gStreamSampleRate > 0) ? gStreamSampleRate : 48000;

    std::vector<float> hiSamples;
    bool hiOk = LoadWavFromAsset(mgr, cHi, hiSamples, targetRate);
    env->ReleaseStringUTFChars(tickHiPath, cHi);

    if (!hiOk || hiSamples.empty()) {
        LOGE("nativeLoadSoundPack: failed to load tickHi");
        return JNI_FALSE;
    }

    std::vector<float> loSamples;
    bool loOk = false;
    if (tickLoPath != nullptr) {
        const char* cLo = env->GetStringUTFChars(tickLoPath, nullptr);
        if (cLo != nullptr) {
            if (cLo[0] != '\0') {
                LOGI("nativeLoadSoundPack: lo=%s", cLo);
                loOk = LoadWavFromAsset(mgr, cLo, loSamples, targetRate);
            }
            env->ReleaseStringUTFChars(tickLoPath, cLo);
        }
    }

    if (!loOk || loSamples.empty()) {
        loSamples = hiSamples;  // 弱拍缺失：复用强拍（单音色）
    }

    gEngine.LoadSoundPack(hiSamples.data(), hiSamples.size(),
                          loSamples.data(), loSamples.size());
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativeTriggerChime(JNIEnv * /*env*/, jclass /*clazz*/) {
    LOGI("nativeTriggerChime");
    gEngine.TriggerChime();
}

// ========== JNI — XRun 监控 ==========

extern "C" JNIEXPORT jint JNICALL
Java_com_runbeat_audio_AudioEngine_nativeGetXRunCount(JNIEnv* /*env*/, jclass /*clazz*/) {
    if (gStream != nullptr) {
        return static_cast<jint>(AAudioStream_getXRunCount(gStream));
    }
    return -1;
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
