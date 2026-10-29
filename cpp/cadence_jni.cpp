#include <jni.h>

#include <algorithm>
#include <cstdint>

#include "core/CadenceDetector.hpp"

namespace {

cadence::CadenceDetector gDetector;
constexpr int kSnapshotFields = 9;

} // namespace

extern "C" {

JNIEXPORT void JNICALL Java_com_runbeat_audio_CadenceNative_resetRaw(JNIEnv*, jclass) {
    gDetector.Reset();
}

JNIEXPORT void JNICALL Java_com_runbeat_audio_CadenceNative_setTargetRaw(JNIEnv*, jclass, jdouble spm) {
    gDetector.SetTarget(static_cast<double>(spm));
}

JNIEXPORT void JNICALL Java_com_runbeat_audio_CadenceNative_setSampleRateRaw(JNIEnv*, jclass, jdouble hz) {
    gDetector.SetSampleRate(static_cast<double>(hz));
}

JNIEXPORT void JNICALL Java_com_runbeat_audio_CadenceNative_pushRaw(
        JNIEnv*, jclass, jfloat x, jfloat y, jfloat z, jlong tsNanos) {
    gDetector.Push(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z),
                   static_cast<std::int64_t>(tsNanos));
}

JNIEXPORT jdoubleArray JNICALL Java_com_runbeat_audio_CadenceNative_snapshotRaw(JNIEnv* env, jclass) {
    const auto snapshot = gDetector.GetSnapshot();
    const jdouble values[kSnapshotFields] = {
            snapshot.cadenceSpm,
            snapshot.avgSpm,
            snapshot.maxSpm,
            snapshot.accuracy,
            snapshot.activeSec,
            static_cast<jdouble>(snapshot.steps),
            snapshot.targetSpm,
            snapshot.tracking ? 1.0 : 0.0,
            snapshot.envelope,
    };
    jdoubleArray out = env->NewDoubleArray(kSnapshotFields);
    if (out != nullptr) {
        env->SetDoubleArrayRegion(out, 0, kSnapshotFields, values);
    }
    return out;
}

JNIEXPORT jint JNICALL Java_com_runbeat_audio_CadenceNative_historyRaw(
        JNIEnv* env, jclass, jfloatArray out) {
    if (out == nullptr) return 0;
    const jsize length = env->GetArrayLength(out);
    const int capacity = std::min<int>(length, cadence::CadenceDetector::kHistorySize);
    if (capacity <= 0) return 0;
    float buffer[cadence::CadenceDetector::kHistorySize] = {};
    const int valid = gDetector.CopyHistory(buffer, capacity);
    env->SetFloatArrayRegion(out, 0, capacity, buffer);
    return valid;
}

} // extern "C"
