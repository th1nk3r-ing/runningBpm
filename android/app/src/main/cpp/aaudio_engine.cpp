#include <jni.h>
#include <android/log.h>
#include <aaudio/AAudio.h>

#include "core/AudioEngine.hpp"

#define LOG_TAG "RunBeat-Native"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// ========== 全局引擎实例 ==========

static AudioEngine gEngine;

// ========== JNI 探针 — 验证 Native 加载 ==========

extern "C" JNIEXPORT jstring JNICALL
Java_com_runbeat_audio_AudioEngine_nativeHello(JNIEnv *env, jclass /*clazz*/) {
    LOGI("native loaded");
    return env->NewStringUTF("hello from native runbeat engine");
}

// ========== 引擎生命周期 ==========

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

// ========== 运行时参数 ==========

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
Java_com_runbeat_audio_AudioEngine_nativeTriggerChime(JNIEnv * /*env*/, jclass /*clazz*/) {
    LOGI("nativeTriggerChime");
    gEngine.TriggerChime();
}
