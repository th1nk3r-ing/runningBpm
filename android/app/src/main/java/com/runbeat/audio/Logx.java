package com.runbeat.audio;

import android.content.Context;
import android.util.Log;

import java.io.File;

/** 统一日志门面：native 可用时转发到 C++ 侧 spdlog，否则回落 android.util.Log。 */
public final class Logx {

    // 与 spdlog::level::level_enum 同序（trace=0 … critical=5）
    public static final int TRACE = 0;
    public static final int DEBUG = 1;
    public static final int INFO = 2;
    public static final int WARN = 3;
    public static final int ERROR = 4;
    public static final int CRITICAL = 5;

    private static volatile boolean nativeReady;

    private Logx() {}

    public static synchronized void init(Context context, boolean writeToFile) {
        if (!nativeReady && AudioEngine.isAvailable()) {
            // 目录建不出来也要初始化 native，否则 logcat 与每拍打点一起消失
            String logDir = "";
            try {
                File dir = new File(context.getFilesDir(), "logs");
                if (dir.isDirectory() || dir.mkdirs()) logDir = dir.getAbsolutePath();
            } catch (Throwable t) {
                logDir = "";
            }
            try {
                nativeReady = nativeInitLoggingRaw(logDir, writeToFile);
            } catch (Throwable t) {
                nativeReady = false;
            }
        }
        setFileEnabled(writeToFile);
    }

    /** 面板开关：关闭后仍打 logcat，仅停止写文件。 */
    public static void setFileEnabled(boolean enabled) {
        if (!nativeReady) return;
        try {
            nativeSetFileLoggingRaw(enabled);
        } catch (Throwable t) {
            nativeReady = false;
        }
    }

    public static void t(String tag, String msg) { log(TRACE, tag, msg); }

    public static void d(String tag, String msg) { log(DEBUG, tag, msg); }

    public static void i(String tag, String msg) { log(INFO, tag, msg); }

    public static void w(String tag, String msg) { log(WARN, tag, msg); }

    public static void e(String tag, String msg) { log(ERROR, tag, msg); }

    private static void log(int level, String tag, String msg) {
        if (nativeReady) {
            try {
                nativeLogRaw(level, tag, msg);
                return;
            } catch (Throwable t) {
                nativeReady = false;
            }
        }
        switch (level) {
            case TRACE:    Log.v(tag, msg); break;
            case DEBUG:    Log.d(tag, msg); break;
            case INFO:     Log.i(tag, msg); break;
            case WARN:     Log.w(tag, msg); break;
            case CRITICAL: Log.wtf(tag, msg); break;
            default:       Log.e(tag, msg); break;
        }
    }

    private static native boolean nativeInitLoggingRaw(String logDir, boolean fileEnabled);

    private static native void nativeSetFileLoggingRaw(boolean enabled);

    private static native void nativeLogRaw(int level, String tag, String message);
}
