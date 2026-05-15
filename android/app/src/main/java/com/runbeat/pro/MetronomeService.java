package com.runbeat.pro;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
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

    private static final String CHANNEL_ID = "metronome_channel";
    private static final int NOTIFICATION_ID = 1001;

    /** WakeLock 最长持有时间：4 小时，超时自动释放，防止进程被杀后 CPU 永不休眠 */
    private static final long WAKE_LOCK_TIMEOUT_MS = 4 * 3600 * 1000L;

    private PowerManager.WakeLock wakeLock;

    // 当前通知显示状态（由 onStartCommand 通过 intent extras 更新）
    private double currentBpm = 180.0;
    private boolean currentPaused = false;

    @Override
    public void onCreate() {
        super.onCreate();
        createNotificationChannel();
        acquireWakeLock();
        startForeground(NOTIFICATION_ID, buildNotification());
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null) {
            double bpm = intent.getDoubleExtra("bpm", currentBpm);
            boolean paused = intent.getBooleanExtra("paused", currentPaused);
            if (bpm != currentBpm || paused != currentPaused) {
                currentBpm = bpm;
                currentPaused = paused;
                // 更新常驻通知内容
                NotificationManager mgr = getSystemService(NotificationManager.class);
                if (mgr != null) mgr.notify(NOTIFICATION_ID, buildNotification());
            }
        }
        return START_STICKY;
    }

    @Nullable
    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onDestroy() {
        releaseWakeLock();
        super.onDestroy();
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

        String state = currentPaused ? "已暂停" : "运行中";
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
}
