package com.runbeat.audio;

/**
 * JNI 桥接类 — Native C++ 音频引擎接口。
 * Phase 1.2: 空壳声明；Phase 2 实现 native 方法。
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

    public static native String nativeHello();

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

    public static native void nativeTriggerChime();

    // ========== 监控 ==========

    /** 获取 AAudio XRun 计数 */
    public static native int nativeGetXRunCount();

    /** 检查 AAudio 流是否因设备断开而死亡，如是则重建。由主线程定时调用。*/
    public static native boolean nativeRebuildStreamIfNeeded();
}
