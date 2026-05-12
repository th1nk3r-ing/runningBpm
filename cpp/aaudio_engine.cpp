#include <jni.h>
#include <android/log.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <aaudio/AAudio.h>
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

// ========== 采样生成 ==========

/** 生成指数衰减脉冲作为默认 tick 采样（无人耳可闻的 click） */
static std::vector<float> GenerateTickSample(int sampleRate) {
    constexpr float kDurationSec = 0.03f;   // 30ms
    constexpr float kFreqHz = 880.0f;
    constexpr float kDecay = 80.0f;          // 衰减系数
    int length = static_cast<int>(sampleRate * kDurationSec);
    std::vector<float> data(length);
    for (int i = 0; i < length; ++i) {
        float t = static_cast<float>(i) / sampleRate;
        data[i] = std::sin(2.0f * 3.14159265f * kFreqHz * t)
                * std::exp(-t * kDecay);
    }
    return data;
}

/** 线性插值 pitch shift — ratio < 1 降调（样本数增加），ratio > 1 升调 */
static std::vector<float> PitchShift(const float* input, size_t inputLen, double ratio) {
    if (ratio <= 0.0 || std::abs(ratio - 1.0) < 1e-9) {
        return std::vector<float>(input, input + inputLen);
    }
    size_t outLen = static_cast<size_t>(inputLen / ratio);
    std::vector<float> output(outLen);
    for (size_t i = 0; i < outLen; ++i) {
        double srcPos = i * ratio;
        size_t idx = static_cast<size_t>(srcPos);
        double frac = srcPos - idx;
        if (idx + 1 < inputLen) {
            output[i] = static_cast<float>(
                input[idx] * (1.0 - frac) + input[idx + 1] * frac);
        } else {
            output[i] = input[idx];
        }
    }
    return output;
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

static aaudio_data_callback_result_t dataCallback(
        AAudioStream* /*stream*/, void* /*userData*/, void* audioData, int32_t numFrames) {
    gEngine.OnAudioCallback(static_cast<float*>(audioData), numFrames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void errorCallback(AAudioStream* /*stream*/, void* /*userData*/, aaudio_result_t error) {
    LOGI("stream error: %d", error);
    if (error == AAUDIO_ERROR_DISCONNECTED) {
        // Phase 3 实现：自动重建流
        LOGI("stream disconnected — will rebuild on next start");
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

    // 查询实际采样率并同步引擎
    int32_t sampleRate = AAudioStream_getSampleRate(gStream);
    gEngine.SetSampleRate(sampleRate);
    LOGI("stream opened: %d Hz, %s mode",
         sampleRate,
         AAudioStream_getSharingMode(gStream) == AAUDIO_SHARING_MODE_EXCLUSIVE
             ? "EXCLUSIVE" : "SHARED");

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

// ========== JNI — 引擎生命周期 ==========

extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativeInit(JNIEnv* /*env*/, jclass /*clazz*/) {
    LOGI("nativeInit");

    // 生成默认 tick 采样
    auto tickSamples = GenerateTickSample(48000);
    gEngine.LoadTickSamples(tickSamples.data(), tickSamples.size());

    // 打开 AAudio 流
    OpenStream();
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
Java_com_runbeat_audio_AudioEngine_nativeLoadSoundPack(
        JNIEnv* env, jclass /*clazz*/,
        jobject assetManagerObj,
        jstring tickHiPath, jdouble pitchRatio) {
    AAssetManager* mgr = AAssetManager_fromJava(env, assetManagerObj);
    if (!mgr) return;

    const char* cPath = env->GetStringUTFChars(tickHiPath, nullptr);
    LOGI("nativeLoadSoundPack: %s (ratio=%.2f)", cPath, static_cast<double>(pitchRatio));

    std::vector<float> samples;
    bool ok = LoadWavFromAsset(mgr, cPath, samples, 48000);
    env->ReleaseStringUTFChars(tickHiPath, cPath);

    if (!ok || samples.empty()) {
        LOGE("nativeLoadSoundPack: failed to load %s", cPath);
        return;
    }

    auto tickLo = PitchShift(samples.data(), samples.size(),
                             static_cast<double>(pitchRatio));
    gEngine.LoadSoundPack(samples.data(), samples.size(),
                          tickLo.data(), tickLo.size());
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

// ========== JNI — 清理 ==========

extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativeDestroy(JNIEnv* /*env*/, jclass /*clazz*/) {
    LOGI("nativeDestroy");
    gEngine.Stop();
    CloseStream();
}
