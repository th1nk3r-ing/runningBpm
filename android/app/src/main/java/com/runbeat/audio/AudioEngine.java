package com.runbeat.audio;

import android.util.Log;

/**
 * JNI 桥接 — native 实现在 cpp/aaudio_engine.cpp；*Raw 私有，包装方法先查
 * {@link #isAvailable()}，库加载失败时降级为 no-op / false 而非抛 UnsatisfiedLinkError。
 */
public class AudioEngine {

    private static final String TAG = "AudioEngine";

    private static boolean libraryLoaded;
    private static boolean initialized;

    static {
        try {
            System.loadLibrary("runbeat");
            libraryLoaded = true;
        } catch (Throwable t) {
            Log.e(TAG, "Failed to load librunbeat.so — audio disabled", t);
        }
    }

    private AudioEngine() {}

    public static boolean isAvailable() {
        return libraryLoaded;
    }

    public static synchronized boolean isInitialized() {
        return initialized;
    }

    public static synchronized boolean initialize() {
        if (!libraryLoaded) return false;
        if (!initialized) initialized = nativeInitRaw();
        return initialized;
    }

    public static synchronized void destroy() {
        if (libraryLoaded) nativeDestroyRaw();
        initialized = false;
    }

    // ========== JNI 原始绑定（库未加载时不得直接调用） ==========

    private static native boolean nativeInitRaw();

    private static native boolean nativeLoadWavAssetsRaw(
            android.content.res.AssetManager mgr,
            String tickHiPath, String tickLoPath, String chimePath);

    private static native void nativeDestroyRaw();

    private static native boolean nativeStartRaw(double bpm);

    private static native void nativeStopRaw();

    private static native void nativePauseRaw();

    private static native boolean nativeResumeRaw();

    private static native int nativeGetStateRaw();

    private static native void nativeSetBpmRaw(double bpm);

    private static native void nativeSetTickVolumeRaw(double vol);

    private static native void nativeSetChimeVolumeRaw(double vol);

    private static native void nativeSetAccentModeRaw(int mode);

    private static native void nativeSetOutputGainRaw(double gain);

    private static native boolean nativeLoadSoundPackRaw(
            android.content.res.AssetManager mgr,
            String tickHiPath, String tickLoPath);

    private static native void nativeTriggerChimeRaw();

    private static native int nativeGetXRunCountRaw();

    private static native long nativeGetBeatSeqRaw();

    private static native void nativeSetMixWithOthersRaw(boolean mixWithOthers);

    private static native boolean nativeRebuildStreamIfNeededRaw();

    // ========== 安全包装（库缺失时降级） ==========

    public static boolean nativeLoadWavAssets(
            android.content.res.AssetManager mgr,
            String tickHiPath, String tickLoPath, String chimePath) {
        return libraryLoaded
                && nativeLoadWavAssetsRaw(mgr, tickHiPath, tickLoPath, chimePath);
    }

    // ========== 引擎生命周期 ==========

    public static synchronized boolean nativeStart(double bpm) {
        if (!libraryLoaded) return false;
        boolean ok = nativeStartRaw(bpm);
        // nativeStart 自行开流，成功后必须同步 initialized，否则调用方会重复重载采样
        if (ok) initialized = true;
        return ok;
    }

    public static void nativeStop() {
        if (libraryLoaded) nativeStopRaw();
    }

    public static void nativePause() {
        if (libraryLoaded) nativePauseRaw();
    }

    public static boolean nativeResume() {
        return libraryLoaded && nativeResumeRaw();
    }

    /** Native state values: 0=Idle, 1=Running, 2=Paused. */
    public static int nativeGetState() {
        return libraryLoaded ? nativeGetStateRaw() : 0;
    }

    // ========== 运行时参数 ==========

    public static void nativeSetBpm(double bpm) {
        if (libraryLoaded) nativeSetBpmRaw(bpm);
    }

    public static void nativeSetTickVolume(double vol) {
        if (libraryLoaded) nativeSetTickVolumeRaw(vol);
    }

    /** PLANNED（整点报时）：Java 层未接线，接口保留待启用。 */
    public static void nativeSetChimeVolume(double vol) {
        if (libraryLoaded) nativeSetChimeVolumeRaw(vol);
    }

    /** 重音模式：0=全部重音 1=全部轻音 2=每 2 拍重音 3=每 4 拍重音 */
    public static void nativeSetAccentMode(int mode) {
        if (libraryLoaded) nativeSetAccentModeRaw(mode);
    }

    /** 输出总倍率（x1/x2/x3），叠加在 tick/chime gain 之上 */
    public static void nativeSetOutputGain(double gain) {
        if (libraryLoaded) nativeSetOutputGainRaw(gain);
    }

    // ========== 音色切换 ==========

    /** 运行时替换 tick 样本；tickLoPath 为 null/空串则两路复用强拍。 */
    public static boolean nativeLoadSoundPack(
            android.content.res.AssetManager mgr,
            String tickHiPath, String tickLoPath) {
        return libraryLoaded && nativeLoadSoundPackRaw(mgr, tickHiPath, tickLoPath);
    }

    // ========== 事件触发 ==========

    /** PLANNED（整点报时）：Java 层未接线，接口保留待启用。 */
    public static void nativeTriggerChime() {
        if (libraryLoaded) nativeTriggerChimeRaw();
    }

    // ========== 监控 ==========

    /** XRun（丢帧）累计次数；未开流或库未加载返回 -1。 */
    public static int nativeGetXRunCount() {
        return libraryLoaded ? nativeGetXRunCountRaw() : -1;
    }

    public static long nativeGetBeatSeq() {
        return libraryLoaded ? nativeGetBeatSeqRaw() : 0L;
    }

    /** 开流前调用：true → 固定 SHARED 混音，false → EXCLUSIVE 优先、失败回退 SHARED。 */
    public static void nativeSetMixWithOthers(boolean mixWithOthers) {
        if (libraryLoaded) nativeSetMixWithOthersRaw(mixWithOthers);
    }

    /** 流因设备断开而死亡时重建；由主线程定时调用。 */
    public static boolean nativeRebuildStreamIfNeeded() {
        return libraryLoaded && nativeRebuildStreamIfNeededRaw();
    }
}
