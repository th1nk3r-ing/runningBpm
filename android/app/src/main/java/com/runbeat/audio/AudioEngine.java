package com.runbeat.audio;

/**
 * JNI 桥接类 — Native C++ 音频引擎接口。
 * Phase 1.2: 空壳声明；Phase 2 实现 native 方法。
 */
public class AudioEngine {

    static {
        System.loadLibrary("runbeat");
    }

    // ========== 初始化和清理 ==========

    /** 初始化引擎、生成采样、打开 AAudio 流 */
    public static native void nativeInit();

    /** 从 Assets 加载 WAV 采样文件 */
    public static native boolean nativeLoadWavAssets(
            android.content.res.AssetManager mgr,
            String tickHiPath, String tickLoPath, String chimePath);

    /** 释放引擎和 AAudio 流 */
    public static native void nativeDestroy();

    // ========== JNI 探针 ==========

    public static native String nativeHello();

    // ========== 引擎生命周期 ==========

    public static native void nativeStart(double bpm);

    public static native void nativeStop();

    public static native void nativePause();

    public static native void nativeResume();

    // ========== 运行时参数 ==========

    public static native void nativeSetBpm(double bpm);

    public static native void nativeSetTickVolume(double vol);

    public static native void nativeSetChimeVolume(double vol);

    public static native void nativeSetAccent(boolean on);

    // ========== 音色切换 ==========

    /** 运行时替换 tick 样本（从 Assets 加载 + pitch shift 生成弱拍） */
    public static native void nativeLoadSoundPack(
            android.content.res.AssetManager mgr,
            String tickHiPath, double pitchRatio);

    // ========== 事件触发 ==========

    public static native void nativeTriggerChime();

    // ========== 监控 ==========

    /** 获取 AAudio XRun 计数 */
    public static native int nativeGetXRunCount();
}
