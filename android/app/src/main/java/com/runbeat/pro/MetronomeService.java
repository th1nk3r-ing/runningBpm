package com.runbeat.pro;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.media.AudioManager;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.util.Log;

import androidx.annotation.Nullable;
import androidx.core.app.NotificationCompat;
import androidx.core.content.ContextCompat;

import com.runbeat.audio.AudioEngine;

import java.util.Locale;
import java.util.concurrent.CopyOnWriteArrayList;

/** 前台服务：常驻通知（targetSdk 34）+ 4h WakeLock + 通过 onStartCommand extras 更新通知。 */
public class MetronomeService extends Service {

    private static volatile MetronomeService instance;

    private static final String CHANNEL_ID = "metronome_channel";
    private static final int NOTIFICATION_ID = 1001;
    private static final String TAG = "RunBeat-Service";
    public static final String ACTION_STOP = "com.runbeat.pro.action.STOP";
    private static final String RUNTIME_PREFS = "metronome_runtime";
    private static final String KEY_RUNNING = "running";
    private static final String KEY_BPM = "bpm";
    /** 新格式：double 位模式，避免 0.1 精度被 float 截断。 */
    private static final String KEY_BPM_BITS = "bpm_bits";
    private static final String KEY_PAUSED = "paused";
    private static final String KEY_PAUSED_BY_CALL = "paused_by_call";
    private static final String KEY_EXPLICIT_STOP = "explicit_stop";

    /** 播放状态观察者：来电暂停/恢复由 Service 驱动，Activity 据此同步 UI（主线程）。 */
    public interface PlaybackStateListener {
        void onServicePlaybackStateChanged(boolean running, boolean paused);
    }

    private static final CopyOnWriteArrayList<PlaybackStateListener> stateListeners =
            new CopyOnWriteArrayList<>();

    public static void addPlaybackStateListener(PlaybackStateListener listener) {
        if (listener != null) stateListeners.addIfAbsent(listener);
    }

    public static void removePlaybackStateListener(PlaybackStateListener listener) {
        stateListeners.remove(listener);
    }

    /** 通话权限授予后补注册监听；Service 未运行时为空操作（onCreate 会再注册）。 */
    public static void onPhonePermissionGranted() {
        MetronomeService s = instance;
        if (s != null) s.startCallMonitoring();
    }

    public static boolean hasPersistedPlayback(Context context) {
        return context.getSharedPreferences(RUNTIME_PREFS, Context.MODE_PRIVATE)
                .getBoolean(KEY_RUNNING, false);
    }

    public static boolean wasPersistedPaused(Context context) {
        return context.getSharedPreferences(RUNTIME_PREFS, Context.MODE_PRIVATE)
                .getBoolean(KEY_PAUSED, false);
    }

    public static double getPersistedBpm(Context context) {
        SharedPreferences prefs = context.getSharedPreferences(RUNTIME_PREFS, Context.MODE_PRIVATE);
        double bpm;
        if (prefs.contains(KEY_BPM_BITS)) {
            bpm = Double.longBitsToDouble(prefs.getLong(KEY_BPM_BITS, 0L));
        } else {
            // 旧版 float 键迁移（只读）
            bpm = prefs.getFloat(KEY_BPM, 180.0f);
        }
        // 损坏/旧格式可能带来 NaN/Inf：通知与引擎都不该看到这种值。
        return (Double.isNaN(bpm) || Double.isInfinite(bpm) || bpm <= 0.0) ? 180.0 : bpm;
    }

    public static void stopExplicitly(Context context) {
        SharedPreferences prefs = context.getSharedPreferences(RUNTIME_PREFS, Context.MODE_PRIVATE);
        boolean serviceActive = instance != null;
        SharedPreferences.Editor editor = prefs.edit()
                .putBoolean(KEY_RUNNING, false)
                .putBoolean(KEY_PAUSED, false)
                .putBoolean(KEY_PAUSED_BY_CALL, false);
        if (serviceActive) {
            editor.putBoolean(KEY_EXPLICIT_STOP, true);
        } else {
            editor.remove(KEY_EXPLICIT_STOP);
        }
        editor.apply();
        context.stopService(new Intent(context, MetronomeService.class));
    }

    /** WakeLock 最长持有时间：4 小时，超时自动释放，防止进程被杀后 CPU 永不休眠 */
    private static final long WAKE_LOCK_TIMEOUT_MS = 4 * 3600 * 1000L;

    private PowerManager.WakeLock wakeLock;

    private double currentBpm = 180.0;
    private boolean currentPaused = false;
    private boolean currentRunning;
    /** 本次暂停是否由来电触发；仅此标记为 true 时通话结束才自动恢复。 */
    private boolean pausedByCall;
    private SharedPreferences runtimePrefs;
    private PreferencesManager preferences;
    private CallStateMonitor callStateMonitor;

    // ===== 耳机拔出监听 =====
    // 不申请音频焦点：节拍器要与音乐/播客共存；中断由 BECOMING_NOISY + 通话监听覆盖
    private boolean noisyReceiverRegistered;

    /** 拔耳机/断蓝牙：必须立即停声，否则会从扬声器以当前倍率外放。 */
    private final BroadcastReceiver noisyReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (AudioManager.ACTION_AUDIO_BECOMING_NOISY.equals(intent.getAction())) {
                Log.i(TAG, "audio becoming noisy — pausing");
                if (currentPaused && pausedByCall) {
                    // 通话中拔耳机：取消自动恢复，否则挂断后会从扬声器外放
                    pausedByCall = false;
                    persistPlaybackState();
                    return;
                }
                pauseForInterruption(false);
            }
        }
    };
    private final Handler streamHealthHandler = new Handler(Looper.getMainLooper());
    private final Runnable streamHealthRunnable = new Runnable() {
        @Override
        public void run() {
            if (!currentRunning) return;
            // 重建前刷新混音偏好，避免其它 App 已开始播放却仍重开 EXCLUSIVE
            AudioEngineRestorer.applyAudioSharingPreference(MetronomeService.this);
            int xruns = AudioEngine.nativeGetXRunCount();
            Log.i("RunBeat-XRun", "xruns=" + xruns);
            AudioEngine.nativeRebuildStreamIfNeeded();
            streamHealthHandler.postDelayed(this, 1000);
        }
    };

    @Override
    public void onCreate() {
        super.onCreate();
        instance = this;
        runtimePrefs = getSharedPreferences(RUNTIME_PREFS, MODE_PRIVATE);
        currentRunning = runtimePrefs.getBoolean(KEY_RUNNING, false);
        currentBpm = getPersistedBpm(this);
        currentPaused = currentRunning && runtimePrefs.getBoolean(KEY_PAUSED, false);
        pausedByCall = currentRunning && runtimePrefs.getBoolean(KEY_PAUSED_BY_CALL, false);
        preferences = new PreferencesManager(this);
        createNotificationChannel();
        startForeground(NOTIFICATION_ID, buildNotification());
        // Service 是通话监听唯一持有者：进程被杀 + START_STICKY 重启后仍能自动暂停
        startCallMonitoring();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_STOP.equals(intent.getAction())) {
            handleStopRequest();
            return START_NOT_STICKY;
        }
        if (intent == null) {
            restorePersistedPlayback();
        } else {
            double bpm = intent.getDoubleExtra("bpm", currentBpm);
            boolean paused = intent.getBooleanExtra("paused", currentPaused);
            currentRunning = intent.getBooleanExtra("running", true);
            currentBpm = bpm;
            currentPaused = paused;
            // UI 同步一律视为用户意图：清除来电暂停标记
            pausedByCall = false;
                persistPlaybackState();
        }
        streamHealthHandler.removeCallbacks(streamHealthRunnable);
        if (currentRunning) streamHealthHandler.postDelayed(streamHealthRunnable, 1000);
        syncPlaybackGuards();
        if (!currentRunning) {
            stopSelf(startId);
            return START_NOT_STICKY;
        }
        updateNotification();
        return START_STICKY;
    }

    @Nullable
    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onDestroy() {
        currentRunning = false;
        // System teardown preserves recovery; only a marked explicit stop clears it here.
        if (runtimePrefs != null && runtimePrefs.getBoolean(KEY_EXPLICIT_STOP, false)) {
            currentPaused = false;
            pausedByCall = false;
            persistPlaybackState();
            runtimePrefs.edit().remove(KEY_EXPLICIT_STOP).apply();
        }
        if (callStateMonitor != null) {
            callStateMonitor.stop();
            callStateMonitor = null;
        }
        streamHealthHandler.removeCallbacks(streamHealthRunnable);
        unregisterNoisyReceiver();
        releaseWakeLock();
        if (instance == this) instance = null;
        super.onDestroy();
    }

    /**
     * 从最近任务划掉 = 显式停止。否则 KEY_RUNNING 残留，下次冷启动会未按键就出声。
     */
    @Override
    public void onTaskRemoved(Intent rootIntent) {
        Log.i(TAG, "task removed — stopping playback");
        handleStopRequest();
        super.onTaskRemoved(rootIntent);
    }

    /** 通知栏「停止」：结束播放、释放引擎并清除可恢复运行态；须在主线程调用。 */
    private void handleStopRequest() {
        Log.i(TAG, "stop requested");
        AudioEngine.nativeStop();
        AudioEngine.destroy();
        currentRunning = false;
        currentPaused = false;
        pausedByCall = false;
        persistPlaybackState();
        runtimePrefs.edit().remove(KEY_EXPLICIT_STOP).apply();
        streamHealthHandler.removeCallbacks(streamHealthRunnable);
        unregisterNoisyReceiver();
        releaseWakeLock();
        notifyPlaybackStateChanged();
        stopForeground(STOP_FOREGROUND_REMOVE);
        stopSelf();
    }

    private void restorePersistedPlayback() {
        currentRunning = runtimePrefs.getBoolean(KEY_RUNNING, false);
        currentBpm = getPersistedBpm(this);
        currentPaused = runtimePrefs.getBoolean(KEY_PAUSED, false);
        pausedByCall = currentRunning && runtimePrefs.getBoolean(KEY_PAUSED_BY_CALL, false);
        if (!currentRunning) return;

        AudioEngineRestorer.applyAudioSharingPreference(this);
        if (!AudioEngine.initialize()) {
            currentRunning = false;
            currentPaused = false;
            pausedByCall = false;
            persistPlaybackState();
            return;
        }

        AudioEngineRestorer.loadDefaultSamples(this);
        AudioEngine.nativeSetBpm(currentBpm);
        AudioEngine.nativeSetTickVolume(preferences.getTickVolume() / 100.0);
        AudioEngine.nativeSetAccentMode(preferences.getAccentMode());
        AudioEngine.nativeSetOutputGain(preferences.getGainLevel());
        AudioEngineRestorer.loadSoundPack(this, preferences.getTimbre());

        boolean restored = AudioEngineRestorer.restorePlaybackState(currentBpm, currentPaused);

        if (!restored) {
            currentRunning = false;
            currentPaused = false;
            pausedByCall = false;
            persistPlaybackState();
        }
    }

    private void persistPlaybackState() {
        if (runtimePrefs == null) return;
        runtimePrefs.edit()
                .putBoolean(KEY_RUNNING, currentRunning)
                .putLong(KEY_BPM_BITS, Double.doubleToRawLongBits(currentBpm))
                .remove(KEY_BPM)
                .putBoolean(KEY_PAUSED, currentPaused)
                .putBoolean(KEY_PAUSED_BY_CALL, pausedByCall)
                .apply();
    }

    private void updateNotification() {
        NotificationManager mgr = getSystemService(NotificationManager.class);
        if (mgr != null) mgr.notify(NOTIFICATION_ID, buildNotification());
    }

    // ===== 通知 =====

    private void createNotificationChannel() {
        NotificationChannel channel = new NotificationChannel(
                CHANNEL_ID,
                getString(R.string.notif_channel_name),
                NotificationManager.IMPORTANCE_LOW);
        channel.setDescription(getString(R.string.notif_channel_desc));
        NotificationManager mgr = getSystemService(NotificationManager.class);
        if (mgr != null) mgr.createNotificationChannel(channel);
    }

    private PendingIntent buildStopPendingIntent() {
        Intent stopIntent = new Intent(this, MetronomeService.class).setAction(ACTION_STOP);
        return PendingIntent.getService(
                this, 2, stopIntent,
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
    }

    private Notification buildNotification() {
        Intent tapIntent = new Intent(this, MainActivity.class);
        tapIntent.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP);
        PendingIntent pendingIntent = PendingIntent.getActivity(
                this, 0, tapIntent,
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);

        String state = !currentRunning
                ? getString(R.string.notif_state_stopped)
                : currentPaused ? getString(R.string.notif_state_paused)
                                : getString(R.string.notif_state_running);
        String text = String.format(Locale.US, "%.1f BPM · %s", currentBpm, state);

        NotificationCompat.Builder builder = new NotificationCompat.Builder(this, CHANNEL_ID)
                .setContentTitle(getString(R.string.app_name))
                .setContentText(text)
                .setSmallIcon(android.R.drawable.ic_media_play)
                .setContentIntent(pendingIntent)
                .setOngoing(true);
        // 用户离开 App 后，通知栏是唯一的停止入口。
        builder.addAction(new NotificationCompat.Action(
                android.R.drawable.ic_menu_close_clear_cancel,
                getString(R.string.notif_action_stop),
                buildStopPendingIntent()));
        return builder.build();
    }

    // ===== 通话状态监听（来电自动暂停的唯一持有者）=====

    /** 创建并注册通话监听；权限未授予时 CallStateMonitor 内部静默降级。 */
    private void startCallMonitoring() {
        if (callStateMonitor == null) {
            callStateMonitor = new CallStateMonitor(this, new CallStateMonitor.Callback() {
                @Override
                public void onCallStarted() {
                    handleCallStarted();
                }

                @Override
                public void onCallEnded() {
                    handleCallEnded();
                }
            });
        }
        callStateMonitor.start();
    }

    private void handleCallStarted() {
        pauseForInterruption(true);
    }

    /** 通话结束：仅当「因来电而暂停」时自动恢复；用户手动暂停/停止不恢复。 */
    private void handleCallEnded() {
        if (!pausedByCall) return;
        pausedByCall = false;
        if (currentRunning) {
            boolean resumed = restorePlaybackState(false);
            if (resumed) currentPaused = false;
        }
        persistPlaybackState();
        syncPlaybackGuards();
        updateNotification();
        notifyPlaybackStateChanged();
    }

    /** 统一的中断暂停（来电 / 拔耳机）；@param byCall true 时通话结束自动恢复 */
    private void pauseForInterruption(boolean byCall) {
        if (!currentRunning || currentPaused) return;
        AudioEngine.nativePause();
        currentPaused = true;
        pausedByCall = byCall;
        persistPlaybackState();
        syncPlaybackGuards();
        updateNotification();
        notifyPlaybackStateChanged();
    }

    private boolean restorePlaybackState(boolean paused) {
        AudioEngineRestorer.applyAudioSharingPreference(this);
        return AudioEngineRestorer.restorePlaybackState(currentBpm, paused);
    }

    private void notifyPlaybackStateChanged() {
        for (PlaybackStateListener listener : stateListeners) {
            listener.onServicePlaybackStateChanged(currentRunning, currentPaused);
        }
    }

    // ===== WakeLock =====

    private void acquireWakeLock() {
        if (wakeLock != null && wakeLock.isHeld()) return;
        PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
        if (pm != null) {
            wakeLock = pm.newWakeLock(
                    PowerManager.PARTIAL_WAKE_LOCK,
                    "RunBeat:MetronomeWakeLock");
            wakeLock.acquire(WAKE_LOCK_TIMEOUT_MS);
        }
    }

    private void releaseWakeLock() {
        if (wakeLock != null && wakeLock.isHeld()) {
            wakeLock.release();
            wakeLock = null;
        }
    }

    private void syncWakeLock() {
        if (currentRunning && !currentPaused) {
            acquireWakeLock();
        } else {
            releaseWakeLock();
        }
    }

    // ===== 耳机拔出 =====

    /** WakeLock 与耳机拔出监听跟随运行态。 */
    private void syncPlaybackGuards() {
        syncWakeLock();
        if (currentRunning && !currentPaused) {
            registerNoisyReceiver();
        } else {
            unregisterNoisyReceiver();
        }
    }

    private void registerNoisyReceiver() {
        if (noisyReceiverRegistered) return;
        ContextCompat.registerReceiver(
                this, noisyReceiver,
                new IntentFilter(AudioManager.ACTION_AUDIO_BECOMING_NOISY),
                ContextCompat.RECEIVER_NOT_EXPORTED);
        noisyReceiverRegistered = true;
    }

    private void unregisterNoisyReceiver() {
        if (!noisyReceiverRegistered) return;
        try {
            unregisterReceiver(noisyReceiver);
        } catch (IllegalArgumentException ignored) {
            // 已注销 / 进程收尾阶段重复调用
        }
        noisyReceiverRegistered = false;
    }
}
