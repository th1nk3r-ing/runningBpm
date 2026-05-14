#include <jni.h>
#include <android/log.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <aaudio/AAudio.h>
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

    int targetRate = 48000;
    std::vector<float> samples;

    // tick_hi
    const char* cTickHi = env->GetStringUTFChars(tickHiPath, nullptr);
    bool tickOk = LoadWavFromAsset(mgr, cTickHi, samples, targetRate);
    env->ReleaseStringUTFChars(tickHiPath, cTickHi);
    if (tickOk) gEngine.LoadTickSamples(samples.data(), samples.size());

    // tick_lo (accent)
    const char* cTickLo = env->GetStringUTFChars(tickLoPath, nullptr);
    if (LoadWavFromAsset(mgr, cTickLo, samples, targetRate)) {
        gEngine.LoadTickLoSamples(samples.data(), samples.size());
    }
    env->ReleaseStringUTFChars(tickLoPath, cTickLo);

    // chime
    const char* cChime = env->GetStringUTFChars(chimePath, nullptr);
    if (LoadWavFromAsset(mgr, cChime, samples, targetRate)) {
        gEngine.LoadChimeSamples(samples.data(), samples.size());
    }
    env->ReleaseStringUTFChars(chimePath, cChime);

    LOGI("WAV assets loaded");
    return JNI_TRUE;
}

// ========== AAudio 回调 ==========

// 复用的 mono 临时缓冲（音频线程独占；首帧分配后 capacity 足够即不再 realloc）
static std::vector<float> gMonoBuf;

static aaudio_data_callback_result_t dataCallback(
        AAudioStream* /*stream*/, void* /*userData*/, void* audioData, int32_t numFrames) {
    float* out = static_cast<float*>(audioData);

    if (gStreamChannels <= 1) {
        gEngine.OnAudioCallback(out, numFrames);
    } else {
        // 多通道（一般为 stereo）：先生成 mono，再交错写入每个通道
        if (static_cast<int32_t>(gMonoBuf.size()) < numFrames) {
            gMonoBuf.assign(static_cast<size_t>(numFrames), 0.0f);
        }
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
    if (error == AAUDIO_ERROR_DISCONNECTED) {
        // 不可在此回调内调用任何 AAudio API，只设标志位
        // 主线程通过 nativeRebuildStreamIfNeeded 轮询并重建
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
    // 强烈建议固定 mono + 48kHz：
    //   1) 我们的引擎按 mono per-sample 写出，必须保证设备 channel=1，否则
    //      多通道的后半 buffer 会读到未初始化内存，产生明显噪音/失真；
    //   2) 固定 48kHz 与样本生成一致，避免 AAudio 内部重采样和 click 音高/速度漂移。
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
static void RebuildStream() {
    LOGI("RebuildStream: closing dead stream");
    CloseStream();                 // 关闭旧流（已断开，stop/close 会快速返回）

    if (!OpenStream()) {           // 重新打开
        LOGE("RebuildStream: OpenStream failed");
        return;
    }

    // 仅当引擎处于 Running 时才重新启动流
    if (gEngine.GetState() == AudioEngine::State::Running) {
        aaudio_result_t result = AAudioStream_requestStart(gStream);
        if (result == AAUDIO_OK) {
            LOGI("RebuildStream: stream restarted");
        } else {
            LOGE("RebuildStream: requestStart failed: %d", result);
        }
    } else {
        LOGI("RebuildStream: engine not running, stream opened but not started");
    }
}

// ========== JNI — 引擎生命周期 ==========

extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativeInit(JNIEnv* /*env*/, jclass /*clazz*/) {
    LOGI("nativeInit");

    // 先打开 AAudio 流，得到设备实际采样率，再按此采样率生成样本，
    // 避免 click 速度/音高随设备 SR 漂移造成的"难听"。
    OpenStream();

    std::vector<float> tickHi, tickLo;
    GenerateDefaultTickSamples(tickHi, tickLo, gStreamSampleRate);
    gEngine.LoadTickSamples(tickHi.data(), tickHi.size());
    gEngine.LoadTickLoSamples(tickLo.data(), tickLo.size());

    if (gStream) {
        AAudioStream_requestStart(gStream);
    }
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_runbeat_audio_AudioEngine_nativeHello(JNIEnv *env, jclass /*clazz*/) {
    LOGI("native loaded");
    return env->NewStringUTF("hello from native runbeat engine");
}

extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativeStart(JNIEnv * /*env*/, jclass /*clazz*/, jdouble bpm) {
    LOGI("nativeStart(%.1f)", bpm);
    gEngine.Start(static_cast<double>(bpm));
}

extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativeStop(JNIEnv * /*env*/, jclass /*clazz*/) {
    LOGI("nativeStop");
    gEngine.Stop();
}

extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativePause(JNIEnv * /*env*/, jclass /*clazz*/) {
    LOGI("nativePause");
    gEngine.Pause();
}

extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativeResume(JNIEnv * /*env*/, jclass /*clazz*/) {
    LOGI("nativeResume");
    gEngine.Resume();
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
 * 替代旧的 pitch-shift 派生方案：直接使用两个独立的 WAV 资源，
 * 保留各自的瞬态特征，适用于打击乐采样（BassDrum1/2、Clap1/2 等）。
 *
 * 若 tickLoPath 为空（null/空串），则两路都使用 tickHiPath（单音色）。
 */
extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativeLoadSoundPack(
        JNIEnv* env, jclass /*clazz*/,
        jobject assetManagerObj,
        jstring tickHiPath, jstring tickLoPath) {
    AAssetManager* mgr = AAssetManager_fromJava(env, assetManagerObj);
    if (!mgr) return;

    const char* cHi = env->GetStringUTFChars(tickHiPath, nullptr);
    LOGI("nativeLoadSoundPack: hi=%s", cHi);

    std::vector<float> hiSamples;
    bool hiOk = LoadWavFromAsset(mgr, cHi, hiSamples, 48000);
    env->ReleaseStringUTFChars(tickHiPath, cHi);

    if (!hiOk || hiSamples.empty()) {
        LOGE("nativeLoadSoundPack: failed to load tickHi");
        return;
    }

    std::vector<float> loSamples;
    bool loOk = false;
    if (tickLoPath != nullptr) {
        const char* cLo = env->GetStringUTFChars(tickLoPath, nullptr);
        if (cLo && cLo[0] != '\0') {
            LOGI("nativeLoadSoundPack: lo=%s", cLo);
            loOk = LoadWavFromAsset(mgr, cLo, loSamples, 48000);
        }
        env->ReleaseStringUTFChars(tickLoPath, cLo);
    }

    if (!loOk || loSamples.empty()) {
        // 弱拍样本缺失或加载失败：复用强拍（单音色音色包）
        loSamples = hiSamples;
    }

    gEngine.LoadSoundPack(hiSamples.data(), hiSamples.size(),
                          loSamples.data(), loSamples.size());
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
    RebuildStream();
    return JNI_TRUE;
}

// ========== JNI — 清理 ==========

extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativeDestroy(JNIEnv* /*env*/, jclass /*clazz*/) {
    LOGI("nativeDestroy");
    gEngine.Stop();
    CloseStream();
}
