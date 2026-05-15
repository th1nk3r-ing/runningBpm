package com.runbeat.pro;

import android.animation.Animator;
import android.animation.AnimatorListenerAdapter;
import android.animation.ObjectAnimator;
import android.animation.ValueAnimator;
import android.content.Intent;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.provider.Settings;
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

import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import java.util.Locale;
import android.content.res.Configuration;
import android.content.res.Resources;

public class MainActivity extends AppCompatActivity {

    // ===== 控件 =====
    private TextView btnLangToggle;
    private View layoutTheme;
    private View btnThemeColor;
    private TextView tvThemeName;
    private View viewStatusDot;
    private TextView tvStatus;
    private TextView btnRestart;
    private Button btnLock;
    private ProgressBar unlockProgress;
    private TextView tvTimer;
    private TextView tvBpm;
    private TextView tvBpmLabel;
    private Button btnBpmMinus5, btnBpmMinus1, btnBpmPlus1, btnBpmPlus5;
    private Button btnStartPause;
    private SeekBar seekTickVolume;
    private TextView btnGain;
    private Switch switchAccent;
    private View layoutTimbre;
    private TextView tvTimbreName;
    private TextView tvTimbreArrow;
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
    private int accentColorIndex = 0;

    // ===== 主题色系 =====
    private static final int[] ACCENT_COLORS = {
        0xFFFF6B35, // 橙焰（默认）
        0xFF00BCD4, // 青电
        0xFF9C27B0, // 紫脉
        0xFF4CAF50, // 绿野
        0xFFE0E0E0, // 白月
    };
    private static final String[] ACCENT_NAMES_ZH = {"橙焰", "青电", "紫脉", "绿野", "白月"};
    private static final String[] ACCENT_NAMES_EN = {"Flame", "Cyan", "Purple", "Green", "White"};

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

    // ===== 音频焦点（Fix #9）=====
    private AudioManager audioManager;
    private AudioFocusRequest audioFocusRequest;

    // ===== 生命周期 =====

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // 加载持久化配置（需要在 super.onCreate 前处理语言）
        prefsManager = new PreferencesManager(this);
        applyLanguage(prefsManager.getLanguage());

        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        bindViews();
        setupListeners();

        // 初始化 Native 引擎（只打开 AAudio 流，不自动 Start）
        AudioEngine.nativeInit();
        AudioEngine.nativeLoadWavAssets(getAssets(), "sounds/default/tick_hi.wav", "sounds/default/tick_lo.wav", "sounds/default/chime.wav");

        // 加载持久化配置（优先级：SharedPreferences < savedInstanceState）
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

        // 恢复主题色
        accentColorIndex = prefsManager.getAccentIndex();
        applyAccentColor(ACCENT_COLORS[accentColorIndex]);

        tvBuildInfo.setText(BuildConfig.GIT_COMMIT + " @ " + BuildConfig.BUILD_TIME);

        // 电池优化引导（Fix #8）：首次 START 前检查，不在这里弹，改在 Start 时检查
        // 此处只在 onCreate 检查一次，避免频繁打扰
        checkBatteryOptimization();
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
        // 释放音频焦点（Fix #9）
        abandonAudioFocus();
        // 停止前台服务（Fix #7：Activity 销毁时服务应跟随停止）
        stopService(new Intent(this, MetronomeService.class));
        AudioEngine.nativeDestroy();
        super.onDestroy();
    }

    // ===== 视图绑定 =====

    private void bindViews() {
        btnLangToggle = findViewById(R.id.btnLangToggle);
        layoutTheme = findViewById(R.id.layoutTheme);
        btnThemeColor = findViewById(R.id.btnThemeColor);
        tvThemeName = findViewById(R.id.tvThemeName);
        viewStatusDot = findViewById(R.id.viewStatusDot);
        tvStatus = findViewById(R.id.tvStatus);
        btnRestart = findViewById(R.id.btnRestart);
        btnLock = findViewById(R.id.btnLock);
        unlockProgress = findViewById(R.id.unlockProgress);
        tvTimer = findViewById(R.id.tvTimer);
        tvBpm = findViewById(R.id.tvBpm);
        tvBpmLabel = findViewById(R.id.tvBpmLabel);
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
        tvTimbreArrow = findViewById(R.id.tvTimbreArrow);
        layoutBpmControls = findViewById(R.id.layoutBpmControls);
        layoutParams = findViewById(R.id.layoutParams);
        tvBuildInfo = findViewById(R.id.tvBuildInfo);
    }

    // ===== 事件注册 =====

    private void setupListeners() {
        btnBpmMinus5.setOnClickListener(v -> adjustBpm(v, -5));
        btnBpmMinus1.setOnClickListener(v -> adjustBpm(v, -1));
        btnBpmPlus1.setOnClickListener(v -> adjustBpm(v, +1));
        btnBpmPlus5.setOnClickListener(v -> adjustBpm(v, +5));

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
        layoutTheme.setOnClickListener(v -> showThemeDialog());
        btnLangToggle.setOnClickListener(v -> toggleLanguage());

        btnLock.setOnClickListener(v -> {
            if (!isLocked) {
                isLocked = true;
                updateLockState();
                Toast.makeText(this, getString(R.string.msg_locked), Toast.LENGTH_SHORT).show();
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

        btnRestart.setOnClickListener(v -> restartMetronome());
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
        String[] names = getResources().getStringArray(R.array.timbre_names);
        new AlertDialog.Builder(this)
                .setTitle(R.string.dialog_timbre_title)
                .setSingleChoiceItems(names, timbreIndex, (dialog, which) -> {
                    loadTimbre(which);
                    dialog.dismiss();
                })
                .setNegativeButton(R.string.btn_cancel, null)
                .show();
    }

    private void loadTimbre(int index) {
        if (index < 0 || index >= SOUND_PACKS.length) return;
        timbreIndex = index;
        String[] names = getResources().getStringArray(R.array.timbre_names);
        String name = names[index];
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
                    Toast.makeText(MainActivity.this, getString(R.string.msg_unlocked), Toast.LENGTH_SHORT).show();
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
                Toast.makeText(MainActivity.this, getString(R.string.msg_auto_locked), Toast.LENGTH_SHORT).show();
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

    /**
     * 调整 BPM 并更新 UI/引擎/通知。
     * @param source 触发操作的 View（用于在正确的 view 上触发触觉反馈）
     * @param delta  BPM 变化量（正负均可）
     */
    private void adjustBpm(View source, int delta) {
        double newBpm = bpm + delta;
        if (newBpm < 120.0) newBpm = 120.0;
        if (newBpm > 220.0) newBpm = 220.0;
        if (newBpm != bpm) {
            bpm = newBpm;
            AudioEngine.nativeSetBpm(bpm);
            updateBpmDisplay();
            animateBpmChange();

            // 触觉反馈：在实际触发操作的 view 上执行，避免在已禁用的按钮上误触发
            source.performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);

            // 持久化 BPM（防抖）
            scheduleBpmSave();

            // 同步服务通知中的 BPM 显示（Fix #22）
            updateServiceNotification();
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
                                adjustBpm(v, delta);
                                startBpmRepeat(v, delta);
                            }
                        }, android.view.ViewConfiguration.getLongPressTimeout());
                        return true;
                    case MotionEvent.ACTION_UP:
                        stopBpmRepeat();
                        v.setPressed(false);
                        if (!isLongPress) adjustBpm(v, delta);
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

    private void startBpmRepeat(View source, int delta) {
        stopBpmRepeat();
        bpmHoldRunnable = new Runnable() {
            @Override
            public void run() {
                adjustBpm(source, delta);
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
            requestAudioFocus();  // Fix #9: 请求音频焦点
            timerHandler.postDelayed(timerRunnable, 1000);
            scheduleAutoLock();
        } else if (isPaused) {
            // RESUME
            isPaused = false;
            AudioEngine.nativeResume();
            requestAudioFocus();  // Fix #9: 恢复时重新请求焦点
            timerHandler.postDelayed(timerRunnable, 1000);
            scheduleAutoLock();
        } else {
            // PAUSE
            isPaused = true;
            AudioEngine.nativePause();
            abandonAudioFocus();  // Fix #9: 暂停时释放焦点
            timerHandler.removeCallbacks(timerRunnable);
            cancelAutoLock();
        }
        updateUI();

        // Fix #7: 前台服务管理
        //   Running: 启动/维持服务
        //   Paused:  更新通知状态（服务继续运行，以便快速 Resume）
        //   (onDestroy 时统一 stopService)
        if (isRunning) {
            Intent svcIntent = new Intent(this, MetronomeService.class);
            svcIntent.putExtra("bpm", bpm);
            svcIntent.putExtra("paused", isPaused);
            startService(svcIntent);
        }
    }

    // ===== RESTART =====

    private void restartMetronome() {
        AudioEngine.nativeStop();
        elapsedSeconds = 0;
        isRunning = true;
        isPaused = false;
        AudioEngine.nativeStart(bpm);
        timerHandler.removeCallbacks(timerRunnable);
        timerHandler.postDelayed(timerRunnable, 1000);
        updateUI();
        scheduleAutoLock();
        updateServiceNotification();
    }

    // ===== 前台服务通知同步（Fix #22）=====

    /**
     * 向 MetronomeService 发送当前 BPM 和暂停状态，触发通知内容更新。
     * 只在服务已启动（isRunning=true）时才有效。
     */
    private void updateServiceNotification() {
        if (!isRunning) return;
        Intent svcIntent = new Intent(this, MetronomeService.class);
        svcIntent.putExtra("bpm", bpm);
        svcIntent.putExtra("paused", isPaused);
        startService(svcIntent);
    }

    // ===== 音频焦点管理（Fix #9）=====

    /**
     * 请求 AUDIOFOCUS_GAIN（长期焦点，适合跑步全程）。
     * 焦点丢失时（如来电结束后对方接管）自动暂停节拍器。
     */
    private void requestAudioFocus() {
        audioManager = (AudioManager) getSystemService(AUDIO_SERVICE);
        if (audioManager == null) return;

        AudioAttributes attrs = new AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                .build();

        audioFocusRequest = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                .setAudioAttributes(attrs)
                .setWillPauseWhenDucked(false)  // 跑步者需要完整音量，不自动降音
                .setOnAudioFocusChangeListener(focusChange -> {
                    if (focusChange == AudioManager.AUDIOFOCUS_LOSS) {
                        // 其他 App 长期占用焦点（如导航语音结束后播放器接管）→ 暂停
                        if (isRunning && !isPaused) {
                            isPaused = true;
                            AudioEngine.nativePause();
                            timerHandler.removeCallbacks(timerRunnable);
                            cancelAutoLock();
                            runOnUiThread(this::updateUI);
                            updateServiceNotification();
                        }
                    }
                    // AUDIOFOCUS_LOSS_TRANSIENT / TRANSIENT_CAN_DUCK：
                    // 短暂打断（如导航提示音），跑步场景保持节拍不暂停。
                }, new Handler(Looper.getMainLooper()))
                .build();

        audioManager.requestAudioFocus(audioFocusRequest);
    }

    /** 放弃音频焦点（暂停或销毁时调用）。 */
    private void abandonAudioFocus() {
        if (audioManager != null && audioFocusRequest != null) {
            audioManager.abandonAudioFocusRequest(audioFocusRequest);
            audioFocusRequest = null;
        }
    }

    // ===== 电池优化引导（Fix #8）=====

    /**
     * 检查是否已在电池优化白名单，否则弹框引导用户添加。
     * 仅在 onCreate 时检查一次，避免每次 START 都打扰。
     */
    private void checkBatteryOptimization() {
        PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
        if (pm == null) return;
        if (!pm.isIgnoringBatteryOptimizations(getPackageName())) {
            new AlertDialog.Builder(this)
                    .setTitle(R.string.dialog_battery_title)
                    .setMessage(R.string.dialog_battery_msg)
                    .setPositiveButton(R.string.btn_go_to_settings, (d, w) -> {
                        try {
                            Intent intent = new Intent(
                                    Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                                    Uri.parse("package:" + getPackageName()));
                            startActivity(intent);
                        } catch (Exception e) {
                            // 部分定制 ROM 不支持此 Intent，静默忽略
                        }
                    })
                    .setNegativeButton("忽略", null)
                    .show();
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
            tvStatus.setText(R.string.status_stopped);
            viewStatusDot.setBackgroundResource(R.drawable.status_dot);
            btnStartPause.setText(R.string.btn_start);
        } else if (isPaused) {
            tvStatus.setText(R.string.status_paused);
            viewStatusDot.setBackgroundResource(R.drawable.status_dot);
            btnStartPause.setText(R.string.btn_resume);
        } else {
            tvStatus.setText(R.string.status_running);
            viewStatusDot.setBackgroundResource(R.drawable.status_dot_active);
            btnStartPause.setText(R.string.btn_pause);
        }
        btnRestart.setVisibility(isRunning ? View.VISIBLE : View.GONE);
    }

    // ===== 主题色 =====

    private void showThemeDialog() {
        AlertDialog.Builder b = new AlertDialog.Builder(this);
        b.setTitle(R.string.dialog_theme_title);
        b.setSingleChoiceItems(
            new BaseAdapter() {
                @Override public int getCount() { return ACCENT_COLORS.length; }
                @Override public Object getItem(int p) { return p; }
                @Override public long getItemId(int p) { return p; }
                @Override public View getView(int pos, View cv, ViewGroup parent) {
                    LinearLayout row = new LinearLayout(MainActivity.this);
                    row.setOrientation(LinearLayout.HORIZONTAL);
                    row.setGravity(Gravity.CENTER_VERTICAL);
                    row.setPadding(dpToPx(20), dpToPx(14), dpToPx(20), dpToPx(14));
                    GradientDrawable d = new GradientDrawable();
                    d.setShape(GradientDrawable.OVAL);
                    d.setColor(ACCENT_COLORS[pos]);
                    View circle = new View(MainActivity.this);
                    int sz = dpToPx(20);
                    LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(sz, sz);
                    lp.setMarginEnd(dpToPx(16));
                    circle.setLayoutParams(lp);
                    circle.setBackground(d);
                    row.addView(circle);
                    TextView tv = new TextView(MainActivity.this);
                    boolean isEn = getResources().getConfiguration().getLocales().get(0).getLanguage().equals("en");
                    tv.setText(isEn ? ACCENT_NAMES_EN[pos] : ACCENT_NAMES_ZH[pos]);
                    tv.setTextColor(pos == accentColorIndex ? ACCENT_COLORS[pos] : 0xFFCCCCCC);
                    tv.setTextSize(16);
                    row.addView(tv);
                    return row;
                }
            },
            accentColorIndex,
            (dialog, which) -> {
                accentColorIndex = which;
                applyAccentColor(ACCENT_COLORS[which]);
                prefsManager.setAccentIndex(which);
                dialog.dismiss();
            }
        );
        b.setNegativeButton("取消", null);
        b.show();
    }

    private void applyAccentColor(int color) {
        // 主题色圆点
        GradientDrawable circleBg = new GradientDrawable();
        circleBg.setShape(GradientDrawable.OVAL);
        circleBg.setColor(color);
        btnThemeColor.setBackground(circleBg);
        // 更新主题名文字
        boolean isEn = getResources().getConfiguration().getLocales().get(0).getLanguage().equals("en");
        tvThemeName.setText(isEn ? ACCENT_NAMES_EN[accentColorIndex] : ACCENT_NAMES_ZH[accentColorIndex]);
        tvThemeName.setTextColor(color);
        // BPM 微调按钮（深色背景 + 主题色边框 + 涟漪）
        for (Button btn : new Button[]{btnBpmMinus5, btnBpmMinus1, btnBpmPlus1, btnBpmPlus5}) {
            btn.setBackground(makeBpmRippleBg(color));
        }
        // START/PAUSE 按钮（主题色实心）
        btnStartPause.setBackground(makeStartPauseBg(color));
        // 倍率按钮
        btnGain.setBackground(makeBpmRippleBg(color));
        btnGain.setTextColor(color);
        // SeekBar
        ColorStateList csl = ColorStateList.valueOf(color);
        seekTickVolume.setThumbTintList(csl);
        seekTickVolume.setProgressTintList(csl);
        // 锁定进度圈（解锁动画的圆弧颜色）
        unlockProgress.setProgressTintList(csl);
        // Switch（选中态为主题色）
        int[][] st = {{android.R.attr.state_checked}, {}};
        switchAccent.setThumbTintList(new ColorStateList(st, new int[]{color, 0xFF888888}));
        int trackOn = Color.argb(128, Color.red(color), Color.green(color), Color.blue(color));
        switchAccent.setTrackTintList(new ColorStateList(st, new int[]{trackOn, 0xFF444444}));
        // 音色行文字 + 外框（重建 ripple 背景以跟随主题色）
        tvTimbreName.setTextColor(color);
        tvTimbreArrow.setTextColor(color);
        layoutTimbre.setBackground(makeTimbreRowBg(color));
        // BPM 数字 + 标签
        tvBpm.setTextColor(color);
        tvBpmLabel.setTextColor(color);
        // RESTART 按钮
        btnRestart.setBackground(makeBpmRippleBg(color));
        btnRestart.setTextColor(color);
    }

    private android.graphics.drawable.Drawable makeBpmRippleBg(int accent) {
        GradientDrawable content = new GradientDrawable();
        content.setShape(GradientDrawable.RECTANGLE);
        content.setColor(0xFF2A2A2A);
        content.setCornerRadius(dpToPx(8));
        content.setStroke(dpToPx(1), accent);
        GradientDrawable mask = new GradientDrawable();
        mask.setShape(GradientDrawable.RECTANGLE);
        mask.setColor(0xFFFFFFFF);
        mask.setCornerRadius(dpToPx(8));
        return new RippleDrawable(ColorStateList.valueOf(accent), content, mask);
    }

    private android.graphics.drawable.Drawable makeStartPauseBg(int accent) {
        GradientDrawable d = new GradientDrawable();
        d.setShape(GradientDrawable.RECTANGLE);
        d.setColor(accent);
        d.setCornerRadius(dpToPx(12));
        return new RippleDrawable(ColorStateList.valueOf(0x40FFFFFF), d, null);
    }

    private android.graphics.drawable.Drawable makeTimbreRowBg(int accent) {
        GradientDrawable content = new GradientDrawable();
        content.setShape(GradientDrawable.RECTANGLE);
        content.setColor(0xFF1E1E1E);
        content.setCornerRadius(dpToPx(10));
        content.setStroke(dpToPx(1), accent);
        int ripple = Color.argb(68, Color.red(accent), Color.green(accent), Color.blue(accent));
        return new RippleDrawable(ColorStateList.valueOf(ripple), content, null);
    }

    private int dpToPx(int dp) {
        return Math.round(dp * getResources().getDisplayMetrics().density);
    }

    private void toggleLanguage() {
        String current = prefsManager.getLanguage();
        String next = current.equals("zh") ? "en" : "zh";
        prefsManager.setLanguage(next);
        
        // 重启 Activity 以应用新语言
        Intent intent = getIntent();
        finish();
        startActivity(intent);
    }

    private void applyLanguage(String lang) {
        Locale locale = new Locale(lang);
        Locale.setDefault(locale);
        Resources res = getResources();
        Configuration config = res.getConfiguration();
        config.setLocale(locale);
        res.updateConfiguration(config, res.getDisplayMetrics());
    }

    private void updateLockState() {
        boolean enabled = !isLocked;
        btnLock.setText(isLocked ? "🔒" : "🔓");
        int accent = ACCENT_COLORS[accentColorIndex];
        int borderColor = isLocked ? Color.argb(102, Color.red(accent), Color.green(accent), Color.blue(accent)) : accent;
        for (Button btn : new Button[]{btnBpmMinus5, btnBpmMinus1, btnBpmPlus1, btnBpmPlus5}) {
            btn.setEnabled(enabled);
            btn.setBackground(makeBpmRippleBg(borderColor));
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
