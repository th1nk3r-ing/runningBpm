package com.runbeat.audio;

/**
 * JNI 桥接类 — Native C++ 音频引擎接口。
 * Phase 1.2: 空壳声明；Phase 2 实现 native 方法。
 */
public class AudioEngine {

    static {
        System.loadLibrary("runbeat");
    }

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

    // ========== 事件触发 ==========

    public static native void nativeTriggerChime();
}
