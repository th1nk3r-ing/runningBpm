package com.runbeat.audio;

/**
 * JNI 桥接类 — Native C++ 音频引擎接口。
 * native 方法由 cpp/aaudio_engine.cpp 实现。
 */
public class AudioEngine {

    private static boolean initialized;

    static {
        System.loadLibrary("runbeat");
    }

    // ========== 初始化和清理 ==========

    /** 初始化引擎、生成采样、打开 AAudio 流 */
    public static native boolean nativeInit();

    /** 从 Assets 加载 WAV 采样文件 */
    public static native boolean nativeLoadWavAssets(
            android.content.res.AssetManager mgr,
            String tickHiPath, String tickLoPath, String chimePath);

    /** 释放引擎和 AAudio 流 */
    public static native void nativeDestroy();

    /** Idempotent lifecycle wrapper; configuration changes keep the native stream alive. */
    public static synchronized boolean isInitialized() {
        return initialized;
    }

    public static synchronized boolean initialize() {
        if (!initialized) {
            initialized = nativeInit();
        }
        return initialized;
    }

    public static synchronized void destroy() {
        // Native start/resume can recover and open a stream even when nativeInit failed.
        // Always destroy the native side; nativeDestroy is idempotent.
        nativeDestroy();
        initialized = false;
    }

    // ========== JNI 探针 ==========
    // （nativeHello 探针已删除：全仓库无调用者。）

    // ========== 引擎生命周期 ==========

    public static native boolean nativeStart(double bpm);

    public static native void nativeStop();

    public static native void nativePause();

    public static native boolean nativeResume();

    /** Native state values: 0=Idle, 1=Running, 2=Paused. */
    public static native int nativeGetState();

    // ========== 运行时参数 ==========

    public static native void nativeSetBpm(double bpm);

    public static native void nativeSetTickVolume(double vol);

    /**
     * 设置 chime 音量。
     * PLANNED（整点报时）：当前 Java 层未接线，接口保留待启用。
     */
    public static native void nativeSetChimeVolume(double vol);

    public static native void nativeSetAccent(boolean on);

    /** 设置输出总倍率（x1 = 1.0, x2 = 2.0, x3 = 3.0），叠加在 tick/chime gain 之上 */
    public static native void nativeSetOutputGain(double gain);

    // ========== 音色切换 ==========

    /**
     * 运行时替换 tick 样本（从 Assets 加载强拍 + 弱拍 WAV）。
     * tickLoPath 可为 null 或空串，则两路使用同一个强拍样本（单音色音色包）。
     */
    public static native boolean nativeLoadSoundPack(
            android.content.res.AssetManager mgr,
            String tickHiPath, String tickLoPath);

    // ========== 事件触发 ==========

    /**
     * 触发一次整点 chime。
     * PLANNED（整点报时）：当前 Java 层未接线，接口保留待启用。
     */
    public static native void nativeTriggerChime();

    // ========== 监控 ==========

    // 调试观测：XRun 由 1Hz 健康检查读取；beatSeq 供核对拍数。

    /** XRun（丢帧）累计次数；未开流返回 -1。 */
    public static native int nativeGetXRunCount();

    /** 本次运行累计拍数。 */
    public static native long nativeGetBeatSeq();

    /**
     * 告知 native 侧「当前是否有其它音频在播放」：true → 固定以 SHARED 开流与系统混音共存，
     * false → 维持 EXCLUSIVE 优先、失败回退 SHARED。需在开流前调用。
     */
    public static native void nativeSetMixWithOthers(boolean mixWithOthers);

    /** 检查 AAudio 流是否因设备断开而死亡，如是则重建。由主线程定时调用。*/
    public static native boolean nativeRebuildStreamIfNeeded();
}
