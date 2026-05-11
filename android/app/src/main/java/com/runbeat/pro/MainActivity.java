package com.runbeat.pro;

import android.animation.ValueAnimator;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;
import android.widget.Button;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;

import com.runbeat.audio.AudioEngine;

public class MainActivity extends AppCompatActivity {

    // ===== 控件 =====
    private View viewStatusDot;
    private TextView tvStatus;
    private Switch switchLock;
    private TextView tvTimer;
    private TextView tvBpm;
    private Button btnBpmMinus5, btnBpmMinus1, btnBpmPlus1, btnBpmPlus5;
    private Button btnStartPause;
    private SeekBar sbTickVolume;
    private Switch switchAccent;
    private View layoutBpmControls, layoutParams;
    private TextView tvBuildInfo;

    // ===== 状态 =====
    private double bpm = 180.0;
    private boolean isRunning = false;
    private boolean isPaused = false;
    private int elapsedSeconds = 0;
    private boolean isLocked = false;

    // ===== 计时器 =====
    private final Handler timerHandler = new Handler(Looper.getMainLooper());
    private final Runnable timerRunnable = new Runnable() {
        @Override
        public void run() {
            elapsedSeconds++;
            updateTimerDisplay();
            if (isRunning && !isPaused) {
                timerHandler.postDelayed(this, 1000);
            }
        }
    };

    // ===== BPM 长按重复 =====
    private final Handler bpmHoldHandler = new Handler(Looper.getMainLooper());
    private Runnable bpmHoldRunnable = null;

    // ===== 解锁长按 =====
    private final Handler unlockHandler = new Handler(Looper.getMainLooper());
    private Runnable unlockRunnable = null;

    // ===== 生命周期 =====

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        bindViews();
        setupListeners();

        // 初始化 Native 引擎
        AudioEngine.nativeInit();
        AudioEngine.nativeLoadWavAssets(getAssets(), "tick_hi.wav", "tick_lo.wav", "chime.wav");

        if (savedInstanceState != null) {
            bpm = savedInstanceState.getDouble("bpm", 180.0);
            elapsedSeconds = savedInstanceState.getInt("elapsed", 0);
            isRunning = savedInstanceState.getBoolean("running", false);
            isPaused = savedInstanceState.getBoolean("paused", false);
        }

        updateUI();

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
        switchLock = findViewById(R.id.switchLock);
        tvTimer = findViewById(R.id.tvTimer);
        tvBpm = findViewById(R.id.tvBpm);
        btnBpmMinus5 = findViewById(R.id.btnBpmMinus5);
        btnBpmMinus1 = findViewById(R.id.btnBpmMinus1);
        btnBpmPlus1 = findViewById(R.id.btnBpmPlus1);
        btnBpmPlus5 = findViewById(R.id.btnBpmPlus5);
        btnStartPause = findViewById(R.id.btnStartPause);
        sbTickVolume = findViewById(R.id.sbTickVolume);
        switchAccent = findViewById(R.id.switchAccent);
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

        sbTickVolume.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                if (fromUser) {
                    float vol = progress / 100.0f;
                    AudioEngine.nativeSetTickVolume(vol);
                }
            }
            @Override public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override public void onStopTrackingTouch(SeekBar seekBar) {}
        });

        switchAccent.setOnCheckedChangeListener((buttonView, isChecked) -> {
            AudioEngine.nativeSetAccent(isChecked);
        });

        switchLock.setOnCheckedChangeListener((buttonView, isChecked) -> {
            if (isChecked) {
                // 锁定：立即生效
                isLocked = true;
                updateLockState();
            } else if (isLocked) {
                // 已锁定状态下尝试关闭 → 驳回，必须长按 2s 解锁
                buttonView.setChecked(true);
                Toast.makeText(this, "长按锁定图标 2 秒解锁", Toast.LENGTH_SHORT).show();
            } else {
                isLocked = false;
                updateLockState();
            }
        });

        switchLock.setOnTouchListener((v, event) -> {
            if (!isLocked) return false;
            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    Toast.makeText(this, "继续按住 2 秒解锁", Toast.LENGTH_SHORT).show();
                    unlockRunnable = () -> {
                        isLocked = false;
                        switchLock.setChecked(false);
                        updateLockState();
                        v.setPressed(false);
                        Toast.makeText(this, "已解锁", Toast.LENGTH_SHORT).show();
                    };
                    unlockHandler.postDelayed(unlockRunnable, 2000);
                    return true;
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_CANCEL:
                    if (unlockRunnable != null) {
                        unlockHandler.removeCallbacks(unlockRunnable);
                        unlockRunnable = null;
                    }
                    v.setPressed(false);
                    return true;
            }
            return true;
        });
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
        } else if (isPaused) {
            // RESUME
            isPaused = false;
            AudioEngine.nativeResume();
            timerHandler.post(timerRunnable);
        } else {
            // PAUSE
            isPaused = true;
            AudioEngine.nativePause();
            timerHandler.removeCallbacks(timerRunnable);
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
        for (int id : new int[]{
                R.id.btnBpmMinus5, R.id.btnBpmMinus1,
                R.id.btnBpmPlus1, R.id.btnBpmPlus5,
        }) {
            findViewById(id).setEnabled(enabled);
        }
        sbTickVolume.setEnabled(enabled);
        switchAccent.setEnabled(enabled);
    }
}
