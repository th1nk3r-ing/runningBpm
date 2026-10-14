package com.runbeat.pro;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;

import androidx.annotation.Nullable;
import androidx.core.app.NotificationCompat;

import com.runbeat.audio.AudioEngine;

import java.util.concurrent.CopyOnWriteArrayList;

/**
 * 前台服务 — 保活三剑客：
 * 1. 常驻通知栏（targetSdk 34 要求）
 * 2. WakeLock 防 CPU 休眠（带超时上限，防止进程异常退出时无限持有）
 * 3. 通知内容动态更新（通过 onStartCommand intent extras 传入 bpm/paused 状态）
 */
public class MetronomeService extends Service {

    private static volatile MetronomeService instance;

    private static final String CHANNEL_ID = "metronome_channel";
    private static final int NOTIFICATION_ID = 1001;
    private static final String RUNTIME_PREFS = "metronome_runtime";
    private static final String KEY_RUNNING = "running";
    private static final String KEY_BPM = "bpm";
    private static final String KEY_PAUSED = "paused";
    private static final String KEY_PAUSED_BY_CALL = "paused_by_call";
    private static final String KEY_EXPLICIT_STOP = "explicit_stop";

    /**
     * 播放状态观察者：来电自动暂停/恢复由 Service 驱动，Activity 在前后台切换时据此同步 UI。
     * 回调在主线程触发。
     */
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

    /**
     * 通话权限授予后由 Activity 调用：补注册通话监听。
     * Service 未运行时为空操作（下次 onCreate 会重新注册）。
     */
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
        return context.getSharedPreferences(RUNTIME_PREFS, Context.MODE_PRIVATE)
                .getFloat(KEY_BPM, 180.0f);
    }

    /** Stop intentionally and mark that this teardown must clear the recovery state. */
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

    // 当前通知显示状态（由 onStartCommand 通过 intent extras 更新）
    private double currentBpm = 180.0;
    private boolean currentPaused = false;
    private boolean currentRunning;
    /** 本次暂停是否由来电触发；仅此标记为 true 时通话结束才自动恢复。 */
    private boolean pausedByCall;
    private SharedPreferences runtimePrefs;
    private PreferencesManager preferences;
    private CallStateMonitor callStateMonitor;
    private final Handler streamHealthHandler = new Handler(Looper.getMainLooper());
    private final Runnable streamHealthRunnable = new Runnable() {
        @Override
        public void run() {
            if (!currentRunning) return;
            // 重建前刷新混音偏好：运行期间其它 App 可能开始播放，避免用旧值重开 EXCLUSIVE。
            AudioEngineRestorer.applyAudioSharingPreference(MetronomeService.this);
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
        currentBpm = runtimePrefs.getFloat(KEY_BPM, 180.0f);
        currentPaused = currentRunning && runtimePrefs.getBoolean(KEY_PAUSED, false);
        pausedByCall = currentRunning && runtimePrefs.getBoolean(KEY_PAUSED_BY_CALL, false);
        preferences = new PreferencesManager(this);
        createNotificationChannel();
        startForeground(NOTIFICATION_ID, buildNotification());
        // 服务是通话监听的唯一持有者：进程被杀 + START_STICKY 重启后依然能自动暂停。
        startCallMonitoring();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null) {
            restorePersistedPlayback();
        } else {
            double bpm = intent.getDoubleExtra("bpm", currentBpm);
            boolean paused = intent.getBooleanExtra("paused", currentPaused);
            currentRunning = intent.getBooleanExtra("running", true);
            currentBpm = bpm;
            currentPaused = paused;
            // 来自 UI 的状态同步一律视为用户意图：清除来电暂停标记，绝不被自动恢复。
            pausedByCall = false;
            persistPlaybackState();
        }
        streamHealthHandler.removeCallbacks(streamHealthRunnable);
        if (currentRunning) streamHealthHandler.postDelayed(streamHealthRunnable, 1000);
        syncWakeLock();
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
        releaseWakeLock();
        if (instance == this) instance = null;
        super.onDestroy();
    }

    private void restorePersistedPlayback() {
        currentRunning = runtimePrefs.getBoolean(KEY_RUNNING, false);
        currentBpm = runtimePrefs.getFloat(KEY_BPM, 180.0f);
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
        AudioEngine.nativeSetAccent(preferences.isAccentEnabled());
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
                .putFloat(KEY_BPM, (float) currentBpm)
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
                "RunBeat Metronome",
                NotificationManager.IMPORTANCE_LOW);
        channel.setDescription("跑步节拍器后台运行通知");
        NotificationManager mgr = getSystemService(NotificationManager.class);
        if (mgr != null) mgr.createNotificationChannel(channel);
    }

    private Notification buildNotification() {
        Intent tapIntent = new Intent(this, MainActivity.class);
        tapIntent.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP);
        PendingIntent pendingIntent = PendingIntent.getActivity(
                this, 0, tapIntent,
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);

        String state = !currentRunning ? "已停止" : currentPaused ? "已暂停" : "运行中";
        String text = String.format("%.0f BPM · %s", currentBpm, state);

        return new NotificationCompat.Builder(this, CHANNEL_ID)
                .setContentTitle("RunBeat Pro")
                .setContentText(text)
                .setSmallIcon(android.R.drawable.ic_media_play)
                .setContentIntent(pendingIntent)
                .setOngoing(true)
                .build();
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

    /** 来电（响铃/接通）：仅在我们运行且未暂停时暂停，并记录暂停来源。 */
    private void handleCallStarted() {
        if (!currentRunning || currentPaused) return;
        AudioEngine.nativePause();
        currentPaused = true;
        pausedByCall = true;
        persistPlaybackState();
        syncWakeLock();
        updateNotification();
        notifyPlaybackStateChanged();
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
        syncWakeLock();
        updateNotification();
        notifyPlaybackStateChanged();
    }

    /** 刷新混音偏好并把引擎带到目标暂停态；供通话结束恢复复用。 */
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
            // 设置 4 小时超时上限：即使 onDestroy 未被调用（进程被强制杀死），
            // WakeLock 也会在超时后自动释放，避免 CPU 无限不休眠导致严重耗电。
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
}
