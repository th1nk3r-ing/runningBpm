package com.runbeat.pro;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
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
    private static final String KEY_EXPLICIT_STOP = "explicit_stop";
    private static volatile boolean recoveryFocusHeld;

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
                .putBoolean(KEY_PAUSED, false);
        if (serviceActive) {
            editor.putBoolean(KEY_EXPLICIT_STOP, true);
        } else {
            editor.remove(KEY_EXPLICIT_STOP);
        }
        editor.apply();
        context.stopService(new Intent(context, MetronomeService.class));
    }

    public static boolean hasRecoveryAudioFocus() {
        return recoveryFocusHeld;
    }

    public static void transferRecoveryAudioFocusToActivity() {
        MetronomeService service = instance;
        if (service != null) service.releaseRecoveryAudioFocus();
    }

    /** WakeLock 最长持有时间：4 小时，超时自动释放，防止进程被杀后 CPU 永不休眠 */
    private static final long WAKE_LOCK_TIMEOUT_MS = 4 * 3600 * 1000L;

    private PowerManager.WakeLock wakeLock;
    private AudioManager audioManager;
    private AudioFocusRequest audioFocusRequest;

    // 当前通知显示状态（由 onStartCommand 通过 intent extras 更新）
    private double currentBpm = 180.0;
    private boolean currentPaused = false;
    private boolean currentRunning;
    private SharedPreferences runtimePrefs;
    private PreferencesManager preferences;
    private final Handler streamHealthHandler = new Handler(Looper.getMainLooper());
    private final Runnable streamHealthRunnable = new Runnable() {
        @Override
        public void run() {
            if (!currentRunning) return;
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
        preferences = new PreferencesManager(this);
        createNotificationChannel();
        startForeground(NOTIFICATION_ID, buildNotification());
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
            persistPlaybackState();
        }
        streamHealthHandler.removeCallbacks(streamHealthRunnable);
        if (currentRunning) streamHealthHandler.postDelayed(streamHealthRunnable, 1000);
        if (!currentRunning || currentPaused) releaseRecoveryAudioFocus();
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
            persistPlaybackState();
            runtimePrefs.edit().remove(KEY_EXPLICIT_STOP).apply();
        }
        streamHealthHandler.removeCallbacks(streamHealthRunnable);
        releaseRecoveryAudioFocus();
        releaseWakeLock();
        if (instance == this) instance = null;
        super.onDestroy();
    }

    private void restorePersistedPlayback() {
        currentRunning = runtimePrefs.getBoolean(KEY_RUNNING, false);
        currentBpm = runtimePrefs.getFloat(KEY_BPM, 180.0f);
        currentPaused = runtimePrefs.getBoolean(KEY_PAUSED, false);
        if (!currentRunning) return;

        if (!currentPaused && !requestRecoveryAudioFocus()) {
            currentRunning = false;
            persistPlaybackState();
            return;
        }

        if (!AudioEngine.initialize()) {
            currentRunning = false;
            currentPaused = false;
            releaseRecoveryAudioFocus();
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
            releaseRecoveryAudioFocus();
            persistPlaybackState();
        }
    }

    private boolean requestRecoveryAudioFocus() {
        if (audioFocusRequest != null) return true;
        audioManager = (AudioManager) getSystemService(AUDIO_SERVICE);
        if (audioManager == null) return false;

        AudioAttributes attributes = new AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                .build();
        audioFocusRequest = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                .setAudioAttributes(attributes)
                .setOnAudioFocusChangeListener(change -> {
                    if (change == AudioManager.AUDIOFOCUS_LOSS && currentRunning && !currentPaused) {
                        AudioEngine.nativePause();
                        currentPaused = true;
                        persistPlaybackState();
                        syncWakeLock();
                        updateNotification();
                        releaseRecoveryAudioFocus();
                    }
                }, new Handler(Looper.getMainLooper()))
                .build();

        if (audioManager.requestAudioFocus(audioFocusRequest) != AudioManager.AUDIOFOCUS_REQUEST_GRANTED) {
            audioManager.abandonAudioFocusRequest(audioFocusRequest);
            audioFocusRequest = null;
            return false;
        }
        recoveryFocusHeld = true;
        return true;
    }

    private void releaseRecoveryAudioFocus() {
        if (audioManager != null && audioFocusRequest != null) {
            audioManager.abandonAudioFocusRequest(audioFocusRequest);
            audioFocusRequest = null;
        }
        recoveryFocusHeld = false;
    }

    private void persistPlaybackState() {
        if (runtimePrefs == null) return;
        runtimePrefs.edit()
                .putBoolean(KEY_RUNNING, currentRunning)
                .putFloat(KEY_BPM, (float) currentBpm)
                .putBoolean(KEY_PAUSED, currentPaused)
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
