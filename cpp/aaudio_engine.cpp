#include <jni.h>
#include <android/log.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/api-level.h>
#include <aaudio/AAudio.h>
#include <dlfcn.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/AudioEngine.hpp"
#include "core/Constants.hpp"
#include "core/WavLoader.hpp"

#define LOG_TAG "RunBeat-Native"
#define LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// 每拍打印一行（音频回调线程内直接调用）；高频，降为 VERBOSE。
static void LogTick(void* /*userData*/, std::uint64_t beatSeq,
                    std::uint64_t framePos, int sampleRate) {
    __android_log_print(ANDROID_LOG_VERBOSE, "RunBeat-Beat",
                        "beat=%llu frame=%llu sr=%d",
                        static_cast<unsigned long long>(beatSeq),
                        static_cast<unsigned long long>(framePos),
                        sampleRate);
}

static AudioEngine gEngine;
static AAudioStream* gStream = nullptr;
static int32_t gStreamSampleRate = beat::kDefaultSampleRate;
static int32_t gStreamChannels = 1;
static std::atomic<bool> gNativeInitialized{false};

// 样本按 gSamplesLoadedRate 保存；采样率变化后据此重采样或重新生成，再推回 gEngine
static std::vector<float> gTickHiSamples;
static std::vector<float> gTickLoSamples;
static std::vector<float> gChimeSamples;
static bool gHaveChimeSamples = false;
static int gSamplesLoadedRate = 0;
static bool gUseAssetSamples = false;  // false = 仅生成式 fallback tick

// 流断开标志：errorCallback（音频线程）写入，主线程检查并重建
static std::atomic<bool> gStreamError{false};

// 其它音频在播时（Java 侧 isMusicActive）固定 SHARED 混音，否则 EXCLUSIVE 优先
static std::atomic<bool> gMixWithOthers{false};

// WAV 失败时的 fallback；sampleRate 必须与流一致，否则音高/时长失真
static void GenerateDefaultTickSamples(std::vector<float>& outTickHi,
                                        std::vector<float>& outTickLo,
                                        int sampleRate) {
    outTickHi = WavGenerator::MakeWoodTick(sampleRate, true);
    outTickLo = WavGenerator::MakeWoodTick(sampleRate, false);
}

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

static int CurrentTargetSampleRate() {
    return (gStreamSampleRate > 0) ? static_cast<int>(gStreamSampleRate)
                                   : beat::kDefaultSampleRate;
}

/** 两个加载入口的公共实现：tickHi 必须成功，tickLo 缺失/失败复用强拍，chime 可跳过。 */
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

/** 采样率变化后重采样音色（音高/时长）；节拍速度由 Clock::SetSampleRate 负责。 */
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

// 复用的 mono 临时缓冲：nativeInit 预 reserve 4096 帧，回调内禁止扩容
static std::vector<float> gMonoBuf;

static aaudio_data_callback_result_t dataCallback(
        AAudioStream* /*stream*/, void* /*userData*/, void* audioData, int32_t numFrames) {
    float* out = static_cast<float*>(audioData);

    if (gStreamChannels <= 1) {
        gEngine.OnAudioCallback(out, numFrames);
    } else {
        // 防御路径：OpenStream 固定 mono，保留以支持未来多声道
        if (gMonoBuf.capacity() < static_cast<size_t>(numFrames)) {
            // 回调内不能扩容：异常大 buffer 直接静音
            const size_t outputSamples = static_cast<size_t>(numFrames) *
                    static_cast<size_t>(gStreamChannels);
            std::fill_n(out, outputSamples, 0.0f);
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        }
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

/** 声明流用途为媒体/音乐；setUsage/setContentType 自 API 28 才有，故 dlsym 取用。 */
static void ApplyMediaStreamAttributes(AAudioStreamBuilder* builder) {
    if (android_get_device_api_level() < 28) return;
    typedef void (*SetUsageFn)(AAudioStreamBuilder*, aaudio_usage_t);
    typedef void (*SetContentTypeFn)(AAudioStreamBuilder*, aaudio_content_type_t);
    static SetUsageFn setUsage =
            reinterpret_cast<SetUsageFn>(dlsym(RTLD_DEFAULT, "AAudioStreamBuilder_setUsage"));
    static SetContentTypeFn setContentType =
            reinterpret_cast<SetContentTypeFn>(dlsym(RTLD_DEFAULT, "AAudioStreamBuilder_setContentType"));
    if (setUsage) setUsage(builder, AAUDIO_USAGE_MEDIA);
    if (setContentType) setContentType(builder, AAUDIO_CONTENT_TYPE_MUSIC);
}

static bool OpenStream() {
    if (gStream != nullptr) return true;

    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK) {
        LOGE("AAudio_createStreamBuilder failed: %d", result);
        return false;
    }

    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    ApplyMediaStreamAttributes(builder);
    // 固定 mono：多通道后半 buffer 会读到未初始化内存
    AAudioStreamBuilder_setChannelCount(builder, 1);
    AAudioStreamBuilder_setSampleRate(builder, beat::kDefaultSampleRate);
    AAudioStreamBuilder_setDataCallback(builder, dataCallback, nullptr);
    AAudioStreamBuilder_setErrorCallback(builder, errorCallback, nullptr);

    // 有其它音频时直接 SHARED；否则先试 EXCLUSIVE 低延迟，失败回退 SHARED
    if (gMixWithOthers.load(std::memory_order_relaxed)) {
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        result = AAudioStreamBuilder_openStream(builder, &gStream);
    } else {
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_EXCLUSIVE);
        result = AAudioStreamBuilder_openStream(builder, &gStream);
        if (result != AAUDIO_OK) {
            LOGI("EXCLUSIVE mode failed (%d), fallback to SHARED", result);
            AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
            result = AAudioStreamBuilder_openStream(builder, &gStream);
        }
    }

    AAudioStreamBuilder_delete(builder);

    if (result != AAUDIO_OK) {
        LOGE("AAudioStreamBuilder_openStream failed: %d", result);
        gStream = nullptr;
        return false;
    }

    gStreamSampleRate = AAudioStream_getSampleRate(gStream);
    gStreamChannels   = AAudioStream_getChannelCount(gStream);
    if (!beat::IsValidSampleRate(gStreamSampleRate)) {
        // 0 会让 Clock 的 BPM 更新静默失效，必须回落默认值
        LOGE("stream returned invalid sample rate %d, falling back to %d",
             gStreamSampleRate, beat::kDefaultSampleRate);
        gStreamSampleRate = beat::kDefaultSampleRate;
    }
    if (gStreamChannels <= 0) {
        LOGE("stream returned invalid channel count %d, assuming mono", gStreamChannels);
        gStreamChannels = 1;
    }
    ReloadSamplesIfSampleRateChanged();
    gEngine.SetSampleRate(gStreamSampleRate);

    // 2 burst 缓冲降低 underrun
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

static bool IsStreamStarted() {
    if (gStream == nullptr) return false;
    const aaudio_stream_state_t state = AAudioStream_getState(gStream);
    return state == AAUDIO_STREAM_STATE_STARTED || state == AAUDIO_STREAM_STATE_STARTING;
}

/** 幂等启动：已 STARTED/STARTING 时直接成功，避免重复 requestStart 触发 INVALID_STATE 拆流。 */
static bool EnsureStreamStarted() {
    if (gStream == nullptr) return false;
    const aaudio_stream_state_t state = AAudioStream_getState(gStream);
    if (state == AAUDIO_STREAM_STATE_STARTED || state == AAUDIO_STREAM_STATE_STARTING) {
        return true;
    }
    if (state == AAUDIO_STREAM_STATE_DISCONNECTED ||
        state == AAUDIO_STREAM_STATE_CLOSING ||
        state == AAUDIO_STREAM_STATE_CLOSED ||
        state == AAUDIO_STREAM_STATE_UNINITIALIZED) {
        LOGE("EnsureStreamStarted: stream not startable (state=%d)", static_cast<int>(state));
        return false;
    }
    const aaudio_result_t r = AAudioStream_requestStart(gStream);
    if (r != AAUDIO_OK) {
        LOGE("EnsureStreamStarted: requestStart failed: %d", r);
        return false;
    }
    return true;
}

static void RequestStreamPause() {
    if (gStream == nullptr) return;
    if (!IsStreamStarted() && AAudioStream_getState(gStream) != AAUDIO_STREAM_STATE_PAUSING) return;
    const aaudio_result_t r = AAudioStream_requestPause(gStream);
    if (r != AAUDIO_OK) LOGE("requestPause failed: %d", r);
}

static void RequestStreamStop() {
    if (gStream == nullptr) return;
    const aaudio_stream_state_t state = AAudioStream_getState(gStream);
    if (state == AAUDIO_STREAM_STATE_OPEN ||
        state == AAUDIO_STREAM_STATE_STOPPED ||
        state == AAUDIO_STREAM_STATE_CLOSED ||
        state == AAUDIO_STREAM_STATE_UNINITIALIZED) {
        return;
    }
    const aaudio_result_t r = AAudioStream_requestStop(gStream);
    if (r != AAUDIO_OK) LOGE("requestStop failed: %d", r);
}

/** 重建流并按引擎状态决定是否启动；必须在非音频线程调用。 */
static bool RebuildStream() {
    LOGI("RebuildStream: closing dead stream");
    CloseStream();

    if (!OpenStream()) {
        // 重建期间可能已有其它 App 播放：以 SHARED 再试一次，失败还原偏好
        const bool prevMix = gMixWithOthers.load(std::memory_order_relaxed);
        LOGI("RebuildStream: OpenStream failed, retrying with SHARED");
        gMixWithOthers.store(true, std::memory_order_relaxed);
        if (!OpenStream()) {
            gMixWithOthers.store(prevMix, std::memory_order_relaxed);
            LOGE("RebuildStream: OpenStream failed (SHARED)");
            return false;
        }
    }

    // 仅 Running 需要流在跑；Paused 保持"已打开未启动"，启动只由 nativeResume 负责
    AudioEngine::State s = gEngine.GetState();
    if (s == AudioEngine::State::Running) {
        if (!EnsureStreamStarted()) return false;
        LOGI("RebuildStream: stream restarted (state=%d)", static_cast<int>(s));
    } else {
        LOGI("RebuildStream: engine state=%d, stream opened but not started",
             static_cast<int>(s));
    }
    return true;
}

/** 初始化流及 fallback 样本；Activity 与 sticky Service 共用。 */
static bool InitializeEngine() {
    if (!OpenStream()) return false;

    if (!gNativeInitialized.load(std::memory_order_acquire)) {
        std::vector<float> tickHi, tickLo;
        GenerateDefaultTickSamples(tickHi, tickLo, gStreamSampleRate);
        gEngine.LoadTickSamples(tickHi.data(), tickHi.size());
        gEngine.LoadTickLoSamples(tickLo.data(), tickLo.size());

        gTickHiSamples = std::move(tickHi);
        gTickLoSamples = std::move(tickLo);
        gSamplesLoadedRate = gStreamSampleRate;
        gUseAssetSamples = false;

        if (gMonoBuf.capacity() < 4096) gMonoBuf.reserve(4096);
        gNativeInitialized.store(true, std::memory_order_release);
    }
    gEngine.SetTickCallback(&LogTick, nullptr);
    return true;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_runbeat_audio_AudioEngine_nativeInitRaw(JNIEnv* /*env*/, jclass /*clazz*/) {
    LOGI("nativeInit");
    return InitializeEngine() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_runbeat_audio_AudioEngine_nativeStartRaw(JNIEnv * /*env*/, jclass /*clazz*/, jdouble bpm) {
    LOGI("nativeStart(%.1f)", bpm);
    if (gStreamError.exchange(false, std::memory_order_acq_rel)) {
        CloseStream();
    }
    if (!InitializeEngine() || gStream == nullptr) {
        LOGE("nativeStart: audio engine initialization failed");
        return JNI_FALSE;
    }
    gEngine.Start(static_cast<double>(bpm));
    if (!EnsureStreamStarted()) {
        LOGE("nativeStart: stream start failed");
        gEngine.Stop();
        gStreamError.store(true, std::memory_order_release);
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativeStopRaw(JNIEnv * /*env*/, jclass /*clazz*/) {
    LOGI("nativeStop");
    gEngine.Stop();
    RequestStreamStop();
}

extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativePauseRaw(JNIEnv * /*env*/, jclass /*clazz*/) {
    LOGI("nativePause");
    gEngine.Pause();
    RequestStreamPause();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_runbeat_audio_AudioEngine_nativeResumeRaw(JNIEnv * /*env*/, jclass /*clazz*/) {
    LOGI("nativeResume");
    if (gStreamError.exchange(false, std::memory_order_acq_rel)) {
        CloseStream();
    }
    if (gStream == nullptr && !InitializeEngine()) return JNI_FALSE;
    if (gStream == nullptr) return JNI_FALSE;
    gEngine.Resume();
    if (!EnsureStreamStarted()) {
        LOGE("nativeResume: stream start failed");
        gEngine.Pause();
        gStreamError.store(true, std::memory_order_release);
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_runbeat_audio_AudioEngine_nativeGetStateRaw(JNIEnv* /*env*/, jclass /*clazz*/) {
    return static_cast<jint>(gEngine.GetState());
}

/** XRun（丢帧）累计次数；未开流返回 -1。 */
extern "C" JNIEXPORT jint JNICALL
Java_com_runbeat_audio_AudioEngine_nativeGetXRunCountRaw(JNIEnv* /*env*/, jclass /*clazz*/) {
    if (gStream == nullptr) return -1;
    return static_cast<jint>(AAudioStream_getXRunCount(gStream));
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_runbeat_audio_AudioEngine_nativeGetBeatSeqRaw(JNIEnv* /*env*/, jclass /*clazz*/) {
    return static_cast<jlong>(gEngine.GetBeatSeq());
}

// setter 多为高频调用，不逐次刷屏；BPM 例外，但也用 VERBOSE（长按会连续触发）。
extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativeSetBpmRaw(JNIEnv * /*env*/, jclass /*clazz*/, jdouble bpm) {
    LOGV("nativeSetBpm(%.3f)", static_cast<double>(bpm));
    gEngine.SetBpm(static_cast<double>(bpm));
}

extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativeSetTickVolumeRaw(JNIEnv * /*env*/, jclass /*clazz*/, jdouble vol) {
    gEngine.SetTickVolume(static_cast<double>(vol));
}

// PLANNED（整点报时）：Java 层暂未接线，接口保留。
extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativeSetChimeVolumeRaw(JNIEnv * /*env*/, jclass /*clazz*/, jdouble vol) {
    gEngine.SetChimeVolume(static_cast<double>(vol));
}

extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativeSetAccentRaw(JNIEnv * /*env*/, jclass /*clazz*/, jboolean on) {
    gEngine.SetAccent(static_cast<bool>(on));
}

extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativeSetOutputGainRaw(JNIEnv * /*env*/, jclass /*clazz*/, jdouble gain) {
    gEngine.SetOutputGain(static_cast<double>(gain));
}

extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativeSetMixWithOthersRaw(
        JNIEnv * /*env*/, jclass /*clazz*/, jboolean mixWithOthers) {
    gMixWithOthers.store(static_cast<bool>(mixWithOthers), std::memory_order_relaxed);
}

/** 契约：hi、lo、（提供的）chime 任一失败即 false；chime 路径缺失不算失败。 */
extern "C" JNIEXPORT jboolean JNICALL
Java_com_runbeat_audio_AudioEngine_nativeLoadWavAssetsRaw(
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

/** 契约：仅要求 tickHi 成功；tickLo 缺失/失败时两路复用强拍（单音色）。 */
extern "C" JNIEXPORT jboolean JNICALL
Java_com_runbeat_audio_AudioEngine_nativeLoadSoundPackRaw(
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
Java_com_runbeat_audio_AudioEngine_nativeTriggerChimeRaw(JNIEnv * /*env*/, jclass /*clazz*/) {
    gEngine.TriggerChime();
}

/** 由 Java 层 1 秒定时器在主线程调用；返回是否重建过流。 */
extern "C" JNIEXPORT jboolean JNICALL
Java_com_runbeat_audio_AudioEngine_nativeRebuildStreamIfNeededRaw(
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

extern "C" JNIEXPORT void JNICALL
Java_com_runbeat_audio_AudioEngine_nativeDestroyRaw(JNIEnv* /*env*/, jclass /*clazz*/) {
    LOGI("nativeDestroy");
    gEngine.Stop();
    CloseStream();
    // 缓存必须与引擎内样本一起失效，否则下次初始化会用合成音覆盖用户音色
    gTickHiSamples.clear();
    gTickLoSamples.clear();
    gChimeSamples.clear();
    gHaveChimeSamples = false;
    gSamplesLoadedRate = 0;
    gUseAssetSamples = false;
    gStreamError.store(false, std::memory_order_relaxed);
    gNativeInitialized.store(false, std::memory_order_release);
}
