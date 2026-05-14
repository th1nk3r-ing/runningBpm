package com.runbeat.pro;

import android.animation.Animator;
import android.animation.AnimatorListenerAdapter;
import android.animation.ObjectAnimator;
import android.animation.ValueAnimator;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;
import android.view.animation.LinearInterpolator;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;

import com.runbeat.audio.AudioEngine;

public class MainActivity extends AppCompatActivity {

    // ===== 控件 =====
    private View viewStatusDot;
    private TextView tvStatus;
    private Button btnLock;
    private ProgressBar unlockProgress;
    private TextView tvTimer;
    private TextView tvBpm;
    private Button btnBpmMinus5, btnBpmMinus1, btnBpmPlus1, btnBpmPlus5;
    private Button btnStartPause;
    private SeekBar seekTickVolume;
    private TextView btnGain;
    private Switch switchAccent;
    private View layoutTimbre;
    private TextView tvTimbreName;
    private View layoutBpmControls, layoutParams;
    private TextView tvBuildInfo;

    // ===== 状态 =====
    private double bpm = 180.0;
    private boolean isRunning = false;
    private boolean isPaused = false;
    private int elapsedSeconds = 0;
    private boolean isLocked = false;
    private int timbreIndex = 0;
    private int gainLevel = 1; // 1=x1, 2=x2, 3=x3
    private int tickVolumePercent = 80;

    // ===== 音色包 =====
    // 每行：{显示名, 强拍 wav, 弱拍 wav（null 表示单音色，与强拍相同）}
    private static final String[][] SOUND_PACKS = {
        {"默认",     "sounds/default/tick_hi.wav",       "sounds/default/tick_lo.wav"},
        {"底鼓",     "sounds/audios/BassDrum1.wav",      "sounds/audios/BassDrum2.wav"},
        {"拍手",     "sounds/audios/Clap1.wav",          "sounds/audios/Clap2.wav"},
        {"响棒",     "sounds/audios/Claves1.wav",        "sounds/audios/Claves2.wav"},
        {"边击",     "sounds/audios/Rimshot1.wav",       "sounds/audios/Rimshot2.wav"},
        {"强弱拍",   "sounds/audios/downbeat.wav",       "sounds/audios/upbeat.wav"},
    };

    // ===== 计时器 =====
    private final Handler timerHandler = new Handler(Looper.getMainLooper());
    private final Runnable timerRunnable = new Runnable() {
        @Override
        public void run() {
            elapsedSeconds++;
            updateTimerDisplay();
            // 检测 AAudio 流断开（耳机拔出/蓝牙切换），若断开则重建流
            AudioEngine.nativeRebuildStreamIfNeeded();
            if (isRunning && !isPaused) {
                timerHandler.postDelayed(this, 1000);
            }
        }
    };

    // ===== BPM 长按重复 =====
    private final Handler bpmHoldHandler = new Handler(Looper.getMainLooper());
    private Runnable bpmHoldRunnable = null;

    // ===== 持久化 =====
    private PreferencesManager prefsManager;

    // ===== BPM 持久化防抖 =====
    private final Handler prefsSaveHandler = new Handler(Looper.getMainLooper());
    private Runnable pendingBpmSave = null;

    // ===== 解锁动画 =====
    private ObjectAnimator unlockAnimator = null;

    // ===== 自动锁定 =====
    private final Handler autoLockHandler = new Handler(Looper.getMainLooper());
    private Runnable autoLockRunnable = null;

    // ===== 生命周期 =====

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        bindViews();
        setupListeners();

        // 初始化 Native 引擎
        AudioEngine.nativeInit();
        AudioEngine.nativeLoadWavAssets(getAssets(), "sounds/default/tick_hi.wav", "sounds/default/tick_lo.wav", "chime.wav");

        // 加载持久化配置（优先级：SharedPreferences < savedInstanceState）
        prefsManager = new PreferencesManager(this);
        bpm = prefsManager.getBpm();
        int savedVolume = prefsManager.getTickVolume();
        boolean savedAccent = prefsManager.isAccentEnabled();

        if (savedInstanceState != null) {
            bpm = savedInstanceState.getDouble("bpm", bpm);
            elapsedSeconds = savedInstanceState.getInt("elapsed", 0);
            isRunning = savedInstanceState.getBoolean("running", false);
            isPaused = savedInstanceState.getBoolean("paused", false);
        }

        tickVolumePercent = savedVolume;
        updateUI();

        // 恢复 UI 控件状态（需要在 updateUI 之后，避免被覆盖）
        seekTickVolume.setProgress(tickVolumePercent);
        switchAccent.setChecked(savedAccent);

        // 同步 Native 引擎（即使未启动，确保参数就绪）
        AudioEngine.nativeSetBpm(bpm);
        AudioEngine.nativeSetTickVolume(tickVolumePercent / 100.0f);

        // 恢复音色
        timbreIndex = prefsManager.getTimbre();
        if (timbreIndex >= 0 && timbreIndex < SOUND_PACKS.length) {
            loadTimbre(timbreIndex);
        }

        // 恢复输出倍率
        gainLevel = prefsManager.getGainLevel();
        applyGainLevel();

        tvBuildInfo.setText(BuildConfig.GIT_COMMIT + " @ " + BuildConfig.BUILD_TIME);
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        outState.putDouble("bpm", bpm);
        outState.putInt("elapsed", elapsedSeconds);
        outState.putBoolean("running", isRunning);
        outState.putBoolean("paused", isPaused);
    }

    @Override
    protected void onDestroy() {
        AudioEngine.nativeDestroy();
        super.onDestroy();
    }

    // ===== 视图绑定 =====

    private void bindViews() {
        viewStatusDot = findViewById(R.id.viewStatusDot);
        tvStatus = findViewById(R.id.tvStatus);
        btnLock = findViewById(R.id.btnLock);
        unlockProgress = findViewById(R.id.unlockProgress);
        tvTimer = findViewById(R.id.tvTimer);
        tvBpm = findViewById(R.id.tvBpm);
        btnBpmMinus5 = findViewById(R.id.btnBpmMinus5);
        btnBpmMinus1 = findViewById(R.id.btnBpmMinus1);
        btnBpmPlus1 = findViewById(R.id.btnBpmPlus1);
        btnBpmPlus5 = findViewById(R.id.btnBpmPlus5);
        btnStartPause = findViewById(R.id.btnStartPause);
        seekTickVolume = findViewById(R.id.seekTickVolume);
        btnGain = findViewById(R.id.btnGain);
        switchAccent = findViewById(R.id.switchAccent);
        layoutTimbre = findViewById(R.id.layoutTimbre);
        tvTimbreName = findViewById(R.id.tvTimbreName);
        layoutBpmControls = findViewById(R.id.layoutBpmControls);
        layoutParams = findViewById(R.id.layoutParams);
        tvBuildInfo = findViewById(R.id.tvBuildInfo);
    }

    // ===== 事件注册 =====

    private void setupListeners() {
        btnBpmMinus5.setOnClickListener(v -> adjustBpm(-5));
        btnBpmMinus1.setOnClickListener(v -> adjustBpm(-1));
        btnBpmPlus1.setOnClickListener(v -> adjustBpm(+1));
        btnBpmPlus5.setOnClickListener(v -> adjustBpm(+5));

        setupBpmHold(btnBpmMinus5, -5);
        setupBpmHold(btnBpmPlus5, +5);

        btnStartPause.setOnClickListener(v -> toggleStartPause());

        seekTickVolume.setProgress(tickVolumePercent);
        seekTickVolume.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar sb, int progress, boolean fromUser) {
                if (fromUser) {
                    tickVolumePercent = progress;
                    AudioEngine.nativeSetTickVolume(tickVolumePercent / 100.0);
                    prefsManager.setTickVolume(tickVolumePercent);
                }
            }
            @Override public void onStartTrackingTouch(SeekBar sb) {
                sb.performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
            }
            @Override public void onStopTrackingTouch(SeekBar sb) {}
        });

        switchAccent.setOnCheckedChangeListener((buttonView, isChecked) -> {
            AudioEngine.nativeSetAccent(isChecked);
            prefsManager.setAccentEnabled(isChecked);
        });

        btnGain.setOnClickListener(v -> {
            gainLevel = gainLevel % 3 + 1; // 1 → 2 → 3 → 1
            applyGainLevel();
            prefsManager.setGainLevel(gainLevel);
        });

        layoutTimbre.setOnClickListener(v -> showTimbreDialog());

        btnLock.setOnClickListener(v -> {
            if (!isLocked) {
                isLocked = true;
                updateLockState();
                Toast.makeText(this, "已锁定", Toast.LENGTH_SHORT).show();
            }
        });

        btnLock.setOnTouchListener((v, event) -> {
            if (!isLocked) return false;
            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    startUnlockProgress();
                    return true;
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_CANCEL:
                    cancelUnlockProgress();
                    v.setPressed(false);
                    return true;
            }
            return false;
        });
    }

    // ===== 输出倍率 =====

    private void applyGainLevel() {
        if (gainLevel < 1) gainLevel = 1;
        if (gainLevel > 3) gainLevel = 3;
        AudioEngine.nativeSetOutputGain((double) gainLevel);
        btnGain.setText("x" + gainLevel);
    }

    // ===== 音色切换 =====

    private void showTimbreDialog() {
        String[] names = new String[SOUND_PACKS.length];
        for (int i = 0; i < SOUND_PACKS.length; i++) {
            names[i] = SOUND_PACKS[i][0];
        }
        new AlertDialog.Builder(this)
                .setTitle("选择音色")
                .setSingleChoiceItems(names, timbreIndex, (dialog, which) -> {
                    loadTimbre(which);
                    dialog.dismiss();
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void loadTimbre(int index) {
        if (index < 0 || index >= SOUND_PACKS.length) return;
        timbreIndex = index;
        String name = SOUND_PACKS[index][0];
        String hiPath = SOUND_PACKS[index][1];
        String loPath = SOUND_PACKS[index][2];
        AudioEngine.nativeLoadSoundPack(getAssets(), hiPath, loPath);
        tvTimbreName.setText(name);
        prefsManager.setTimbre(index);
    }

    // ===== 解锁动画 =====

    private void startUnlockProgress() {
        cancelUnlockProgress();
        unlockProgress.setProgress(0);
        unlockAnimator = ObjectAnimator.ofInt(unlockProgress, "progress", 0, 2000);
        unlockAnimator.setDuration(2000);
        unlockAnimator.setInterpolator(new LinearInterpolator());
        unlockAnimator.addListener(new AnimatorListenerAdapter() {
            @Override
            public void onAnimationEnd(Animator animation) {
                if (isLocked) {
                    isLocked = false;
                    updateLockState();
                    Toast.makeText(MainActivity.this, "已解锁", Toast.LENGTH_SHORT).show();
                    if (isRunning && !isPaused) {
                        scheduleAutoLock();
                    }
                }
            }
        });
        unlockAnimator.start();
    }

    private void cancelUnlockProgress() {
        if (unlockAnimator != null) {
            unlockAnimator.removeAllListeners();
            unlockAnimator.cancel();
            unlockAnimator = null;
        }
        unlockProgress.setProgress(0);
    }

    // ===== 自动锁定 =====

    private void scheduleAutoLock() {
        cancelAutoLock();
        autoLockRunnable = () -> {
            if (isRunning && !isPaused && !isLocked) {
                isLocked = true;
                updateLockState();
                Toast.makeText(MainActivity.this, "已自动锁定", Toast.LENGTH_SHORT).show();
            }
        };
        autoLockHandler.postDelayed(autoLockRunnable, 60_000);
    }

    private void cancelAutoLock() {
        if (autoLockRunnable != null) {
            autoLockHandler.removeCallbacks(autoLockRunnable);
            autoLockRunnable = null;
        }
    }

    // ===== BPM 调节 =====

    private void adjustBpm(int delta) {
        double newBpm = bpm + delta;
        if (newBpm < 150.0) newBpm = 150.0;
        if (newBpm > 190.0) newBpm = 190.0;
        if (newBpm != bpm) {
            bpm = newBpm;
            AudioEngine.nativeSetBpm(bpm);
            updateBpmDisplay();
            animateBpmChange();

            // 触觉反馈
            btnBpmMinus5.performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);

            // 持久化 BPM（防抖）
            scheduleBpmSave();
        }
    }

    private void setupBpmHold(Button btn, int delta) {
        btn.setOnTouchListener(new View.OnTouchListener() {
            private boolean isLongPress = false;

            @Override
            public boolean onTouch(View v, MotionEvent event) {
                switch (event.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        isLongPress = false;
                        stopBpmRepeat();
                        v.setPressed(true);
                        v.postDelayed(() -> {
                            if (v.isPressed()) {
                                isLongPress = true;
                                adjustBpm(delta);
                                startBpmRepeat(delta);
                            }
                        }, android.view.ViewConfiguration.getLongPressTimeout());
                        return true;
                    case MotionEvent.ACTION_UP:
                        stopBpmRepeat();
                        v.setPressed(false);
                        if (!isLongPress) adjustBpm(delta);
                        return true;
                    case MotionEvent.ACTION_CANCEL:
                        stopBpmRepeat();
                        v.setPressed(false);
                        return true;
                }
                return false;
            }
        });
    }

    private void startBpmRepeat(int delta) {
        stopBpmRepeat();
        bpmHoldRunnable = new Runnable() {
            @Override
            public void run() {
                adjustBpm(delta);
                bpmHoldHandler.postDelayed(this, 100);
            }
        };
        bpmHoldHandler.postDelayed(bpmHoldRunnable, 300);
    }

    private void stopBpmRepeat() {
        if (bpmHoldRunnable != null) {
            bpmHoldHandler.removeCallbacks(bpmHoldRunnable);
            bpmHoldRunnable = null;
        }
        // 长按结束时立即持久化 BPM
        if (pendingBpmSave != null) {
            prefsSaveHandler.removeCallbacks(pendingBpmSave);
            prefsManager.setBpm(bpm);
            pendingBpmSave = null;
        }
    }

    // ===== BPM 持久化防抖 =====

    private void scheduleBpmSave() {
        if (pendingBpmSave != null) {
            prefsSaveHandler.removeCallbacks(pendingBpmSave);
        }
        pendingBpmSave = () -> {
            prefsManager.setBpm(bpm);
            pendingBpmSave = null;
        };
        prefsSaveHandler.postDelayed(pendingBpmSave, 300);
    }

    // ===== BPM 数字缩放动画 =====

    private void animateBpmChange() {
        ValueAnimator anim = ValueAnimator.ofFloat(1.0f, 1.25f, 1.0f);
        anim.setDuration(200);
        anim.addUpdateListener(a -> {
            float v = (float) a.getAnimatedValue();
            tvBpm.setScaleX(v);
            tvBpm.setScaleY(v);
        });
        anim.start();
    }

    // ===== START / PAUSE =====

    private void toggleStartPause() {
        if (!isRunning) {
            // START
            isRunning = true;
            isPaused = false;
            elapsedSeconds = 0;
            AudioEngine.nativeStart(bpm);
            timerHandler.post(timerRunnable);
            scheduleAutoLock();
        } else if (isPaused) {
            // RESUME
            isPaused = false;
            AudioEngine.nativeResume();
            timerHandler.post(timerRunnable);
            scheduleAutoLock();
        } else {
            // PAUSE
            isPaused = true;
            AudioEngine.nativePause();
            timerHandler.removeCallbacks(timerRunnable);
            cancelAutoLock();
        }
        updateUI();

        // 前台服务绑定
        if (isRunning && !isPaused) {
            startService(new Intent(this, MetronomeService.class));
        } else if (!isRunning || isPaused) {
            // 暂不停止 Service（后台保持）
            // stopService(new Intent(this, MetronomeService.class));
        }
    }

    // ===== UI 刷新 =====

    private void updateUI() {
        updateBpmDisplay();
        updateTimerDisplay();
        updateStatusDisplay();
        updateLockState();
    }

    private void updateBpmDisplay() {
        tvBpm.setText(String.valueOf((int) bpm));
    }

    private void updateTimerDisplay() {
        int h = elapsedSeconds / 3600;
        int m = (elapsedSeconds % 3600) / 60;
        int s = elapsedSeconds % 60;
        tvTimer.setText(String.format("%02d:%02d:%02d", h, m, s));
    }

    private void updateStatusDisplay() {
        if (!isRunning) {
            tvStatus.setText("STOPPED");
            viewStatusDot.setBackgroundResource(R.drawable.status_dot);
            btnStartPause.setText("START");
        } else if (isPaused) {
            tvStatus.setText("PAUSED");
            viewStatusDot.setBackgroundResource(R.drawable.status_dot);
            btnStartPause.setText("RESUME");
        } else {
            tvStatus.setText("RUNNING");
            viewStatusDot.setBackgroundResource(R.drawable.status_dot_active);
            btnStartPause.setText("PAUSE");
        }
    }

    private void updateLockState() {
        boolean enabled = !isLocked;
        btnLock.setText(isLocked ? "🔒" : "🔓");
        for (int id : new int[]{
                R.id.btnBpmMinus5, R.id.btnBpmMinus1,
                R.id.btnBpmPlus1, R.id.btnBpmPlus5,
        }) {
            findViewById(id).setEnabled(enabled);
        }
        seekTickVolume.setEnabled(enabled);
        seekTickVolume.setAlpha(enabled ? 1.0f : 0.4f);
        btnGain.setEnabled(enabled);
        btnGain.setAlpha(enabled ? 1.0f : 0.4f);
        switchAccent.setEnabled(enabled);
        layoutTimbre.setEnabled(enabled);
        layoutTimbre.setAlpha(enabled ? 1.0f : 0.4f);
    }
}
