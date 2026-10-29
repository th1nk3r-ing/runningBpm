package com.runbeat.pro;

import android.content.Context;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Process;

import com.runbeat.audio.CadenceNative;
import com.runbeat.audio.Logx;

import java.util.Locale;

/**
 * 加速度采集壳层 — 只负责注册传感器、转发样本、按实际间隔校准采样率并汇总日志。
 * 检测算法与统计全部在 native 的 CadenceDetector 中。
 */
final class CadenceSensor implements SensorEventListener {

    private static final String TAG = "RunBeat-Cadence";
    private static final int SAMPLE_PERIOD_US = 5000;   // 请求 200Hz
    private static final double REQUESTED_HZ = 200.0;
    private static final long NANOS_PER_SEC = 1_000_000_000L;

    private final SensorManager sensorManager;
    private final Sensor accelerometer;

    private HandlerThread thread;
    private Handler handler;
    private boolean running;

    private long rateWindowStartNs;
    private int rateSamples;
    private double actualHz;
    private boolean rateCalibrated;
    private boolean lastTracking;
    private long lastLogNs;

    CadenceSensor(Context context) {
        sensorManager = (SensorManager) context.getSystemService(Context.SENSOR_SERVICE);
        accelerometer = sensorManager == null
                ? null
                : sensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
    }

    boolean isSupported() {
        return sensorManager != null && accelerometer != null && CadenceNative.isAvailable();
    }

    void start() {
        if (running) return;
        if (!isSupported()) {
            Logx.e(TAG, "accelerometer unavailable — cadence disabled");
            return;
        }
        CadenceNative.setSampleRate(REQUESTED_HZ);
        rateWindowStartNs = 0;
        rateSamples = 0;
        actualHz = 0.0;
        rateCalibrated = false;
        lastTracking = false;
        lastLogNs = 0;

        thread = new HandlerThread("runbeat-cadence", Process.THREAD_PRIORITY_DISPLAY);
        thread.start();
        handler = new Handler(thread.getLooper());
        if (!sensorManager.registerListener(this, accelerometer, SAMPLE_PERIOD_US, handler)) {
            Logx.e(TAG, "sensor register failed requestedHz=" + (int) REQUESTED_HZ);
            thread.quitSafely();
            thread = null;
            handler = null;
            return;
        }
        running = true;
        Logx.i(TAG, "sensor started requestedHz=" + (int) REQUESTED_HZ);
    }

    void stop() {
        if (!running) return;
        sensorManager.unregisterListener(this, accelerometer);
        if (thread != null) {
            thread.quitSafely();
            thread = null;
        }
        handler = null;
        running = false;
        lastTracking = false;
        Logx.i(TAG, "sensor stopped");
    }

    @Override
    public void onSensorChanged(SensorEvent event) {
        if (event.sensor.getType() != Sensor.TYPE_ACCELEROMETER) return;
        final long ts = event.timestamp;
        CadenceNative.push(event.values[0], event.values[1], event.values[2], ts);
        trackSampleRate(ts);
        logSummary(ts);
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) {}

    /** 前 1 秒按实际平均间隔估计采样率，偏差超过 20% 时让 native 重算带通。 */
    private void trackSampleRate(long ts) {
        if (rateWindowStartNs == 0) {
            rateWindowStartNs = ts;
            rateSamples = 0;
        }
        ++rateSamples;
        final long span = ts - rateWindowStartNs;
        if (span < NANOS_PER_SEC) return;

        actualHz = rateSamples * (double) NANOS_PER_SEC / span;
        if (!rateCalibrated) {
            rateCalibrated = true;
            if (Math.abs(actualHz - REQUESTED_HZ) > REQUESTED_HZ * 0.2) {
                CadenceNative.setSampleRate(actualHz);
                Logx.w(TAG, String.format(Locale.US,
                        "rate rebalanced requested=%.1f actual=%.1f", REQUESTED_HZ, actualHz));
            } else {
                Logx.i(TAG, String.format(Locale.US,
                        "rate calibrated requested=%.1f actual=%.1f", REQUESTED_HZ, actualHz));
            }
        }
        rateWindowStartNs = ts;
        rateSamples = 0;
    }

    /** 跟踪边界即时打点；采样行在跟踪中 1s、空闲时 5s 一次。 */
    private void logSummary(long ts) {
        final double[] s = CadenceNative.snapshot();
        final boolean tracking = s[CadenceNative.TRACKING] != 0;
        if (tracking != lastTracking) {
            lastTracking = tracking;
            if (tracking) {
                Logx.i(TAG, String.format(Locale.US, "tracking acquired spm=%.1f steps=%d",
                        s[CadenceNative.CADENCE], (long) s[CadenceNative.STEPS]));
            } else {
                Logx.i(TAG, String.format(Locale.US, "tracking lost steps=%d sec=%.0f",
                        (long) s[CadenceNative.STEPS], s[CadenceNative.ACTIVE_SEC]));
            }
        }

        final long interval = tracking ? NANOS_PER_SEC : 5 * NANOS_PER_SEC;
        if (lastLogNs != 0 && ts - lastLogNs < interval) return;
        lastLogNs = ts;
        Logx.d(TAG, String.format(Locale.US,
                "cadence=%.1f avg=%.1f max=%.1f acc=%.2f steps=%d env=%.2f tracking=%d requestedHz=%.0f actualHz=%.0f",
                s[CadenceNative.CADENCE], s[CadenceNative.AVG], s[CadenceNative.MAX],
                s[CadenceNative.ACCURACY], (long) s[CadenceNative.STEPS], s[CadenceNative.ENVELOPE],
                tracking ? 1 : 0, REQUESTED_HZ, actualHz));
    }
}
