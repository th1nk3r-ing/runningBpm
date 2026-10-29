package com.runbeat.audio;

/**
 * 步频检测 JNI 桥接 — native 实现在 cpp/cadence_jni.cpp。
 * 与 {@link AudioEngine} 相同：库未加载时公开方法降级为空操作。
 */
public final class CadenceNative {

    private static final String TAG = "CadenceNative";
    public static final int SNAPSHOT_FIELDS = 9;

    public static final int CADENCE = 0;
    public static final int AVG = 1;
    public static final int MAX = 2;
    public static final int ACCURACY = 3;
    public static final int ACTIVE_SEC = 4;
    public static final int STEPS = 5;
    public static final int TARGET = 6;
    public static final int TRACKING = 7;
    public static final int ENVELOPE = 8;

    private static boolean libraryLoaded;

    static {
        try {
            System.loadLibrary("runbeat");
            libraryLoaded = true;
        } catch (Throwable t) {
            android.util.Log.e(TAG, "Failed to load librunbeat.so — cadence disabled", t);
        }
    }

    private CadenceNative() {}

    public static boolean isAvailable() {
        return libraryLoaded;
    }

    public static void reset() {
        if (libraryLoaded) resetRaw();
    }

    public static void setTarget(double spm) {
        if (libraryLoaded) setTargetRaw(spm);
    }

    /** 传入实测采样率，用于重算带通系数。 */
    public static void setSampleRate(double hz) {
        if (libraryLoaded) setSampleRateRaw(hz);
    }

    public static void push(float x, float y, float z, long tsNanos) {
        if (libraryLoaded) pushRaw(x, y, z, tsNanos);
    }

    public static double[] snapshot() {
        return libraryLoaded ? snapshotRaw() : new double[SNAPSHOT_FIELDS];
    }

    /** 返回有效点数，out 中 0.0 表示该秒无数据。 */
    public static int history(float[] out) {
        return libraryLoaded ? historyRaw(out) : 0;
    }

    private static native void resetRaw();

    private static native void setTargetRaw(double spm);

    private static native void setSampleRateRaw(double hz);

    private static native void pushRaw(float x, float y, float z, long tsNanos);

    private static native double[] snapshotRaw();

    private static native int historyRaw(float[] out);
}
