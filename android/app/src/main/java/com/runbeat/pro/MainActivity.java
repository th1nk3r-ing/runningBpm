package com.runbeat.pro;

import android.Manifest;
import android.animation.Animator;
import android.animation.AnimatorListenerAdapter;
import android.animation.ObjectAnimator;
import android.animation.ValueAnimator;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.os.SystemClock;
import android.util.TypedValue;
import android.provider.Settings;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.view.animation.LinearInterpolator;
import android.widget.Button;
import android.widget.ImageButton;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.content.ContextCompat;
import androidx.core.view.GravityCompat;
import androidx.drawerlayout.widget.DrawerLayout;

import com.runbeat.audio.AudioEngine;
import com.runbeat.audio.CadenceNative;
import com.runbeat.audio.Logx;

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
    private DrawerLayout drawerLayout;
    private ImageButton btnSettings;
    private TextView btnCloseSettings;
    private View drawerLang;
    private TextView tvLangValue;
    private View drawerTheme;
    private View btnThemeColor;
    private TextView tvThemeName;
    private TextView tvThemeArrow;
    private View viewStatusDot;
    private TextView tvStatus;
    private TextView btnRestart;
    private Button btnLock;
    private ProgressBar unlockProgress;
    private TextView tvTimer;
    private TextView tvPrimaryValue;
    private TextView tvLabelPrimary;
    private View layoutTargetRow;
    private TextView tvTargetValue;
    private TextView tvCadenceStatus;
    private View layoutCadenceTap;
    private Button btnBpmMinus5, btnBpmMinus1, btnBpmPlus1, btnBpmPlus5;
    private Button btnStartPause;
    private SeekBar seekTickVolume;
    private TextView btnGain;
    private View drawerAccent;
    private TextView tvAccentName;
    private TextView tvAccentArrow;
    private int accentMode = 0;
    private View drawerTimbre;
    private View drawerLogToFile;
    private Switch switchLogToFile;
    private TextView tvLogState;
    private TextView tvTimbreName;
    private TextView tvTimbreArrow;
    private View layoutBpmControls;
    private TextView tvBuildInfo;
    private CadenceRingView cadenceRing;
    private TextView tvSteps;
    private TextView btnStats;

    // ===== 状态 =====
    private static final String TAG_UI = "RunBeat-UI";
    private static final String TAG_CADENCE = "RunBeat-Cadence";
    /** BPM 产品区间（120–220，支持 0.1 精度）。 */
    private static final double MIN_BPM = 120.0;
    private static final double MAX_BPM = 220.0;
    private static final int BPM_STEPS_PER_UNIT = 10;   // 0.1 精度

    private double bpm = 180.0;
    private boolean isRunning = false;
    private boolean isPaused = false;
    private int elapsedSeconds = 0;
    private boolean isLocked = false;
    private int timbreIndex = 0;
    private int gainLevel = 1; // 1=x1, 2=x2, 3=x3
    private int tickVolumePercent = 80;
    private int accentColorIndex = 0;
    private boolean soundAssetsLoaded;
    /** onCreate 恢复持久化状态期间抑制「用户操作」日志。 */
    private boolean restoringUi = true;
    /** 长按前的 BPM；只在值真的变了时才记一条。 */
    private double bpmHoldStart;
    /** 拖动前的音量；只在值真的变了时才记一条。 */
    private int volumeAtTouchStart;
    private boolean cadenceSupported;
    private double lastCadenceSpm;
    private long lastBpmAnimMs;
    private int targetValueColor = 0xFF8E8E93;

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

    // ===== 计时器 =====
    private final Handler timerHandler = new Handler(Looper.getMainLooper());
    private final Runnable timerRunnable = new Runnable() {
        @Override
        public void run() {
            elapsedSeconds++;
            updateTimerDisplay();
            updateCadenceUi();
            Logx.i("RunBeat-Stats", "elapsed=" + elapsedSeconds
                    + " beats=" + AudioEngine.nativeGetBeatSeq()
                    + " wallMs=" + SystemClock.elapsedRealtime());
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

    // ===== 来电自动暂停 =====
    // 通话监听归 MetronomeService；Activity 只申请权限并同步状态
    private ActivityResultLauncher<String> phonePermissionLauncher;
    private boolean phonePermissionRequested = false;

    // ===== 通知权限（Android 13+ 前台服务通知） =====
    private ActivityResultLauncher<String> notificationPermissionLauncher;
    private boolean notificationPermissionRequested = false;

    private final MetronomeService.PlaybackStateListener serviceStateListener =
            this::syncUiWithServiceState;

    // ===== 生命周期 =====

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // 语言需在 super.onCreate 前设置
        prefsManager = new PreferencesManager(this);
        applyLanguage(prefsManager.getLanguage());

        super.onCreate(savedInstanceState);
        Logx.init(this, prefsManager.getLogToFile());
        setContentView(R.layout.activity_main);

        bindViews();
        updateLanguageRow();
        switchLogToFile.setChecked(prefsManager.getLogToFile());
        updateLogStateText(switchLogToFile.isChecked());
        setupListeners();
        cadenceSupported = new CadenceSensor(this).isSupported();

        // 权限拒绝只影响来电自动暂停，不影响节拍器
        phonePermissionLauncher = registerForActivityResult(
                new ActivityResultContracts.RequestPermission(),
                granted -> {
                    Logx.i(TAG_UI, "permission phone=" + granted);
                    if (granted) MetronomeService.onPhonePermissionGranted();
                });

        notificationPermissionLauncher = registerForActivityResult(
                new ActivityResultContracts.RequestPermission(),
                granted -> {
                    Logx.i(TAG_UI, "permission notification=" + granted);
                    // 授权前已发出的通知会被系统丢弃且不补发，授权后必须重新 notify
                    if (granted && isRunning) updateServiceNotification();
                });

        AudioEngineRestorer.applyAudioSharingPreference(this);
        boolean engineReady = AudioEngine.initialize();
        if (engineReady) {
            soundAssetsLoaded = AudioEngineRestorer.loadDefaultSamples(this);
        }

        // 优先级：SharedPreferences < savedInstanceState
        bpm = prefsManager.getBpm();
        int savedVolume = prefsManager.getTickVolume();
        accentMode = prefsManager.getAccentMode();

        if (savedInstanceState != null) {
            bpm = savedInstanceState.getDouble("bpm", bpm);
            elapsedSeconds = savedInstanceState.getInt("elapsed", 0);
            isRunning = savedInstanceState.getBoolean("running", false);
            isPaused = savedInstanceState.getBoolean("paused", false);
            isLocked = savedInstanceState.getBoolean("locked", false);
        } else if (MetronomeService.hasPersistedPlayback(this)) {
            bpm = MetronomeService.getPersistedBpm(this);
            isRunning = true;
            isPaused = MetronomeService.wasPersistedPaused(this);
        }

        tickVolumePercent = savedVolume;
        bpm = clampBpm(bpm);
        updateUI();

        seekTickVolume.setProgress(tickVolumePercent);
        applyAccentMode(accentMode);

        AudioEngine.nativeSetBpm(bpm);
        AudioEngine.nativeSetTickVolume(tickVolumePercent / 100.0f);
        CadenceNative.setTarget(bpm);

        timbreIndex = prefsManager.getTimbre();
        if (timbreIndex >= 0 && timbreIndex < AudioEngineRestorer.SOUND_PACKS.length) {
            loadTimbre(timbreIndex);
        }

        gainLevel = prefsManager.getGainLevel();
        applyGainLevel();

        accentColorIndex = prefsManager.getAccentIndex();
        applyAccentColor(ACCENT_COLORS[accentColorIndex]);

        tvBuildInfo.setText(BuildConfig.GIT_COMMIT + " @ " + BuildConfig.BUILD_TIME);

        // 杀进程重建时引擎是新实例；配置变更则沿用原流与相位
        if (isRunning) {
            boolean restored = restoreAudioEngine(bpm, isPaused);
            if (!restored) {
                isRunning = false;
                isPaused = false;
                Logx.w(TAG_UI, "restore failed: audio unavailable");
                Toast.makeText(this, R.string.msg_audio_unavailable, Toast.LENGTH_LONG).show();
            }
        }
        if (isRunning) {
            if (!isPaused) {
                timerHandler.removeCallbacks(timerRunnable);
                timerHandler.postDelayed(timerRunnable, 1000);
            }
            updateUI();
            updateServiceNotification();
        }

        checkBatteryOptimization();
        restoringUi = false;
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        outState.putDouble("bpm", bpm);
        outState.putInt("elapsed", elapsedSeconds);
        outState.putBoolean("running", isRunning);
        outState.putBoolean("paused", isPaused);
        outState.putBoolean("locked", isLocked);
    }

    @Override
    protected void onResume() {
        super.onResume();
        // 后台期间可能被来电暂停/恢复，回前台先对齐状态
        MetronomeService.addPlaybackStateListener(serviceStateListener);
        syncFromPersistedState();
    }

    @Override
    protected void onPause() {
        MetronomeService.removePlaybackStateListener(serviceStateListener);
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        MetronomeService.removePlaybackStateListener(serviceStateListener);
        timerHandler.removeCallbacks(timerRunnable);
        bpmHoldHandler.removeCallbacksAndMessages(null);
        autoLockHandler.removeCallbacksAndMessages(null);
        cancelUnlockProgress();
        // 播放中由前台服务托管：销毁时不能停服务或销毁共享引擎，否则节拍被静默掐断
        if (!isChangingConfigurations() && !isRunning) {
            MetronomeService.stopExplicitly(this);
            AudioEngine.destroy();
        }
        super.onDestroy();
    }

    // ===== 视图绑定 =====

    private void bindViews() {
        drawerLayout = findViewById(R.id.drawerLayout);
        btnSettings = findViewById(R.id.btnSettings);
        btnCloseSettings = findViewById(R.id.btnCloseSettings);
        drawerLang = findViewById(R.id.drawerLang);
        tvLangValue = findViewById(R.id.tvLangValue);
        drawerTheme = findViewById(R.id.drawerTheme);
        btnThemeColor = findViewById(R.id.btnThemeColor);
        tvThemeName = findViewById(R.id.tvThemeName);
        tvThemeArrow = findViewById(R.id.tvThemeArrow);
        viewStatusDot = findViewById(R.id.viewStatusDot);
        tvStatus = findViewById(R.id.tvStatus);
        btnRestart = findViewById(R.id.btnRestart);
        btnLock = findViewById(R.id.btnLock);
        unlockProgress = findViewById(R.id.unlockProgress);
        tvTimer = findViewById(R.id.tvTimer);
        tvPrimaryValue = findViewById(R.id.tvPrimaryValue);
        tvLabelPrimary = findViewById(R.id.tvLabelPrimary);
        layoutTargetRow = findViewById(R.id.layoutTargetRow);
        tvTargetValue = findViewById(R.id.tvTargetValue);
        tvCadenceStatus = findViewById(R.id.tvCadenceStatus);
        layoutCadenceTap = findViewById(R.id.layoutCadenceTap);
        btnBpmMinus5 = findViewById(R.id.btnBpmMinus5);
        btnBpmMinus1 = findViewById(R.id.btnBpmMinus1);
        btnBpmPlus1 = findViewById(R.id.btnBpmPlus1);
        btnBpmPlus5 = findViewById(R.id.btnBpmPlus5);
        btnStartPause = findViewById(R.id.btnStartPause);
        seekTickVolume = findViewById(R.id.seekTickVolume);
        btnGain = findViewById(R.id.btnGain);
        drawerAccent = findViewById(R.id.drawerAccent);
        tvAccentName = findViewById(R.id.tvAccentName);
        tvAccentArrow = findViewById(R.id.tvAccentArrow);
        drawerTimbre = findViewById(R.id.drawerTimbre);
        drawerLogToFile = findViewById(R.id.drawerLogToFile);
        switchLogToFile = findViewById(R.id.switchLogToFile);
        tvLogState = findViewById(R.id.tvLogState);
        tvTimbreName = findViewById(R.id.tvTimbreName);
        tvTimbreArrow = findViewById(R.id.tvTimbreArrow);
        layoutBpmControls = findViewById(R.id.layoutBpmControls);
        tvBuildInfo = findViewById(R.id.tvBuildInfo);
        cadenceRing = findViewById(R.id.cadenceRing);
        tvSteps = findViewById(R.id.tvSteps);
        btnStats = findViewById(R.id.btnStats);
    }

    // ===== 事件注册 =====

    private void setupListeners() {
        btnBpmMinus5.setOnClickListener(v -> adjustBpm(v, -5.0));
        btnBpmMinus1.setOnClickListener(v -> adjustBpm(v, -1.0));
        btnBpmPlus1.setOnClickListener(v -> adjustBpm(v, +1.0));
        btnBpmPlus5.setOnClickListener(v -> adjustBpm(v, +5.0));

        setupBpmHold(btnBpmMinus5, -5.0);
        setupBpmHold(btnBpmPlus5, +5.0);

        // 长按 START/PAUSE = 完全停止
        btnStartPause.setOnClickListener(v -> toggleStartPause());
        btnStartPause.setOnLongClickListener(v -> {
            if (isLocked) return false;
            stopPlayback();
            return true;
        });

        // 点击大号 BPM 数字：0.1 精度精确调整（按钮只做整数步进）。
        layoutCadenceTap.setOnClickListener(v -> {
            if (!isLocked) showBpmDialog();
        });

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
                volumeAtTouchStart = tickVolumePercent;
            }
            @Override public void onStopTrackingTouch(SeekBar sb) {
                if (tickVolumePercent != volumeAtTouchStart) {
                    Logx.i(TAG_UI, "volume=" + tickVolumePercent);
                }
            }
        });

        btnSettings.setOnClickListener(v -> openSettings());
        btnCloseSettings.setOnClickListener(v -> closeSettings());
        drawerLang.setOnClickListener(v -> toggleLanguage());
        drawerTheme.setOnClickListener(v -> showThemeDialog());
        drawerAccent.setOnClickListener(v -> showAccentDialog());

        btnGain.setOnClickListener(v -> {
            gainLevel = gainLevel % 3 + 1; // 1 → 2 → 3 → 1
            applyGainLevel();
            prefsManager.setGainLevel(gainLevel);
        });

        drawerTimbre.setOnClickListener(v -> showTimbreDialog());

        switchLogToFile.setOnCheckedChangeListener((buttonView, isChecked) -> {
            // 先落这条再切 sink：关闭时该行必须在摘挂前写出去
            Logx.i(TAG_UI, "file log " + (isChecked ? "on" : "off"));
            prefsManager.setLogToFile(isChecked);
            Logx.setFileEnabled(isChecked);
            updateLogStateText(isChecked);
        });
        drawerLogToFile.setOnClickListener(v -> switchLogToFile.toggle());

        btnLock.setOnClickListener(v -> {
            if (!isLocked) {
                isLocked = true;
                updateLockState();
                Logx.i(TAG_UI, "lock");
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

        btnStats.setOnClickListener(v -> {
            Logx.i(TAG_UI, "stats");
            CadenceStatsDialog.show(this, ACCENT_COLORS[accentColorIndex]);
        });

        btnRestart.setOnClickListener(v -> restartMetronome());
    }

    // ===== 设置抽屉 =====

    private void openSettings() {
        drawerLayout.openDrawer(GravityCompat.START);
    }

    private void closeSettings() {
        drawerLayout.closeDrawer(GravityCompat.START);
    }

    private void updateLanguageRow() {
        boolean isZh = prefsManager.getLanguage().equals("zh");
        tvLangValue.setText(isZh ? "中文" : "English");
    }

    private void updateLogStateText(boolean enabled) {
        tvLogState.setText(enabled ? R.string.log_on : R.string.log_off);
        tvLogState.setTextColor(enabled
                ? ACCENT_COLORS[accentColorIndex]
                : ContextCompat.getColor(this, R.color.textSecondary));
    }

    // ===== 输出倍率 =====

    private void applyGainLevel() {
        if (gainLevel < 1) gainLevel = 1;
        if (gainLevel > 3) gainLevel = 3;
        AudioEngine.nativeSetOutputGain((double) gainLevel);
        btnGain.setText("x" + gainLevel);
        if (!restoringUi) Logx.i(TAG_UI, "gain=x" + gainLevel);
    }

    // ===== 重音模式 =====

    private void showAccentDialog() {
        String[] names = getResources().getStringArray(R.array.accent_mode_names);
        new AlertDialog.Builder(this)
                .setTitle(R.string.dialog_accent_title)
                .setSingleChoiceItems(names, accentMode, (dialog, which) -> {
                    applyAccentMode(which);
                    dialog.dismiss();
                })
                .setNegativeButton(R.string.btn_cancel, null)
                .show();
    }

    private void applyAccentMode(int mode) {
        if (mode < 0 || mode > 5) return;
        accentMode = mode;
        AudioEngine.nativeSetAccentMode(mode);
        String[] names = getResources().getStringArray(R.array.accent_mode_names);
        tvAccentName.setText(names[mode]);
        prefsManager.setAccentMode(mode);
        if (!restoringUi) Logx.i(TAG_UI, "accent=" + mode + " " + names[mode]);
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
        if (!AudioEngineRestorer.loadSoundPack(this, index)) {
            Logx.w(TAG_UI, "timbre load failed: " + index);
            return;
        }
        timbreIndex = index;
        String[] names = getResources().getStringArray(R.array.timbre_names);
        String name = names[index];
        tvTimbreName.setText(name);
        prefsManager.setTimbre(index);
        if (!restoringUi) Logx.i(TAG_UI, "timbre=" + index + " " + name);
    }

    private void ensureAudioSamplesLoaded() {
        if (soundAssetsLoaded) return;
        soundAssetsLoaded = AudioEngineRestorer.loadDefaultSamples(this);
        AudioEngineRestorer.loadSoundPack(this, timbreIndex);
    }

    private boolean restoreAudioEngine(double targetBpm, boolean paused) {
        // 引擎可能已被 stopPlayback/nativeDestroy 释放：样本缓存随之失效，需重新载入。
        if (!AudioEngine.isInitialized()) soundAssetsLoaded = false;
        AudioEngineRestorer.applyAudioSharingPreference(this);
        boolean restored = AudioEngineRestorer.restorePlaybackState(targetBpm, paused);
        if (restored) ensureAudioSamplesLoaded();
        return restored;
    }

    // ===== 解锁动画 =====

    private void startUnlockProgress() {
        cancelUnlockProgress();
        unlockProgress.setProgress(0);
        unlockAnimator = ObjectAnimator.ofInt(unlockProgress, "progress", 0, 2000);
        unlockAnimator.setDuration(1200);
        unlockAnimator.setInterpolator(new LinearInterpolator());
        unlockAnimator.addListener(new AnimatorListenerAdapter() {
            @Override
            public void onAnimationEnd(Animator animation) {
                if (isLocked) {
                    isLocked = false;
                    updateLockState();
                    Logx.i(TAG_UI, "unlock");
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
                Logx.i(TAG_UI, "auto-lock");
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

    /** 调整 BPM 并同步 UI/引擎/通知。 */
    private void adjustBpm(View source, double delta) {
        double newBpm = clampBpm(bpm + delta);
        if (newBpm != bpm) {
            applyBpm(newBpm);

            source.performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
            scheduleBpmSave();
            updateServiceNotification();
        }
    }

    /** 统一入口：夹取 → 量化到 0.1 → 同步引擎与 UI。 */
    private void applyBpm(double newBpm) {
        if (isLocked) return;
        bpm = clampBpm(newBpm);
        AudioEngine.nativeSetBpm(bpm);
        CadenceNative.setTarget(bpm);
        updateBpmDisplay();
        updateCadenceUi();
        animateBpmChange();
        // 长按连发期间不逐次打点，停手时由 stopBpmRepeat 记一条
        if (bpmHoldRunnable == null) Logx.i(TAG_UI, "bpm=" + bpmText());
    }

    private String bpmText() {
        return String.format(Locale.US, "%.1f", bpm);
    }

    /** 夹取到产品区间并量化到 0.1，避免 179.9999999 之类的浮点尾巴。 */
    private static double clampBpm(double value) {
        if (Double.isNaN(value) || Double.isInfinite(value)) return 180.0;
        double clamped = Math.max(MIN_BPM, Math.min(MAX_BPM, value));
        return Math.round(clamped * BPM_STEPS_PER_UNIT) / (double) BPM_STEPS_PER_UNIT;
    }

    private void showBpmDialog() {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = dpToPx(20);
        box.setPadding(pad, pad, pad, 0);

        TextView valueView = new TextView(this);
        valueView.setTextSize(32);
        valueView.setGravity(Gravity.CENTER);
        valueView.setTextColor(ACCENT_COLORS[accentColorIndex]);
        valueView.setText(String.format(Locale.US, "%.1f", bpm));
        box.addView(valueView);

        SeekBar seek = new SeekBar(this);
        seek.setMax(Math.round((float) ((MAX_BPM - MIN_BPM) * BPM_STEPS_PER_UNIT)));
        seek.setProgress((int) Math.round((bpm - MIN_BPM) * BPM_STEPS_PER_UNIT));
        seek.setProgressTintList(ColorStateList.valueOf(ACCENT_COLORS[accentColorIndex]));
        seek.setThumbTintList(ColorStateList.valueOf(ACCENT_COLORS[accentColorIndex]));
        box.addView(seek);

        seek.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar sb, int progress, boolean fromUser) {
                double value = MIN_BPM + progress / (double) BPM_STEPS_PER_UNIT;
                valueView.setText(String.format(Locale.US, "%.1f", value));
                if (fromUser) sb.performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK);
            }
            @Override public void onStartTrackingTouch(SeekBar sb) {}
            @Override public void onStopTrackingTouch(SeekBar sb) {}
        });

        new AlertDialog.Builder(this)
                .setTitle(R.string.dialog_bpm_title)
                .setView(box)
                .setPositiveButton(R.string.btn_ok, (d, w) -> {
                    applyBpm(MIN_BPM + seek.getProgress() / (double) BPM_STEPS_PER_UNIT);
                    scheduleBpmSave();
                    updateServiceNotification();
                })
                .setNegativeButton(R.string.btn_cancel, null)
                .show();
    }

    private void setupBpmHold(Button btn, double delta) {
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
                                // 先置连发标志，首步与后续步一样不在 applyBpm 里打点
                                startBpmRepeat(v, delta);
                                adjustBpm(v, delta);
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

    private void startBpmRepeat(View source, double delta) {
        stopBpmRepeat();
        bpmHoldStart = bpm;
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
            if (bpm != bpmHoldStart) Logx.i(TAG_UI, "bpm=" + bpmText());
        }
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
        final long now = SystemClock.elapsedRealtime();
        if (now - lastBpmAnimMs < 200) return;
        lastBpmAnimMs = now;
        final View target = isRunning ? tvTargetValue : tvPrimaryValue;
        ValueAnimator anim = ValueAnimator.ofFloat(1.0f, 1.10f, 1.0f);
        anim.setDuration(180);
        anim.addUpdateListener(a -> {
            float v = (float) a.getAnimatedValue();
            target.setScaleX(v);
            target.setScaleY(v);
        });
        anim.start();
    }

    // ===== START / PAUSE =====

    private void toggleStartPause() {
        if (!isRunning) {
            maybeRequestNotificationPermission();
            if (!restoreAudioEngine(bpm, false)) {
                Logx.w(TAG_UI, "start failed: audio unavailable");
                Toast.makeText(this, R.string.msg_audio_unavailable, Toast.LENGTH_LONG).show();
                return;
            }
            Logx.i(TAG_UI, "start bpm=" + bpmText());
            isRunning = true;
            isPaused = false;
            elapsedSeconds = 0;
            resetCadenceStats();
            timerHandler.removeCallbacks(timerRunnable);
            timerHandler.postDelayed(timerRunnable, 1000);
            scheduleAutoLock();
            maybeRequestPhonePermission();
        } else if (isPaused) {
            maybeRequestNotificationPermission();
            if (!restoreAudioEngine(bpm, false)) {
                Logx.w(TAG_UI, "resume failed: audio unavailable");
                Toast.makeText(this, R.string.msg_audio_unavailable, Toast.LENGTH_LONG).show();
                return;
            }
            Logx.i(TAG_UI, "resume bpm=" + bpmText());
            isPaused = false;
            timerHandler.postDelayed(timerRunnable, 1000);
            scheduleAutoLock();
            maybeRequestPhonePermission();
        } else {
            Logx.i(TAG_UI, "pause");
            isPaused = true;
            AudioEngine.nativePause();
            timerHandler.removeCallbacks(timerRunnable);
            cancelAutoLock();
        }
        updateUI();

        // Paused 时服务继续运行以便快速 Resume；真正结束走 stopPlayback()
        if (isRunning) {
            Intent svcIntent = new Intent(this, MetronomeService.class);
            svcIntent.putExtra("bpm", bpm);
            svcIntent.putExtra("paused", isPaused);
            svcIntent.putExtra("running", isRunning);
            startService(svcIntent);
        }
    }

    /** 完全停止：结束计时/前台服务/原生流并清除可恢复运行态。 */
    private void stopPlayback() {
        Logx.i(TAG_UI, "stop");
        AudioEngine.nativeStop();
        // 释放流，避免以 OPEN 状态长期占用 EXCLUSIVE 通路
        AudioEngine.destroy();
        soundAssetsLoaded = false;
        isRunning = false;
        isPaused = false;
        elapsedSeconds = 0;
        resetCadenceStats();
        timerHandler.removeCallbacks(timerRunnable);
        cancelAutoLock();
        cancelUnlockProgress();
        updateUI();
        MetronomeService.stopExplicitly(this);
        Toast.makeText(this, R.string.msg_stopped, Toast.LENGTH_SHORT).show();
    }

    /** Back = 显式停止，后台保活靠 Home 键；否则 KEY_RUNNING 残留会让下次冷启动自动出声。 */
    @Override
    public void onBackPressed() {
        if (drawerLayout.isDrawerOpen(GravityCompat.START)) {
            closeSettings();
            return;
        }
        if (isRunning) {
            stopPlayback();
        }
        super.onBackPressed();
    }

    // ===== RESTART =====

    private void restartMetronome() {
        AudioEngine.nativeStop();
        if (!restoreAudioEngine(bpm, false)) {
            isRunning = false;
            isPaused = false;
            MetronomeService.stopExplicitly(this);
            updateUI();
            Logx.w(TAG_UI, "restart failed: audio unavailable");
            Toast.makeText(this, R.string.msg_audio_unavailable, Toast.LENGTH_LONG).show();
            return;
        }
        Logx.i(TAG_UI, "restart bpm=" + bpmText());
        elapsedSeconds = 0;
        isRunning = true;
        isPaused = false;
        resetCadenceStats();
        timerHandler.removeCallbacks(timerRunnable);
        timerHandler.postDelayed(timerRunnable, 1000);
        updateUI();
        scheduleAutoLock();
        updateServiceNotification();
    }

    // ===== 前台服务通知同步 =====

    private void updateServiceNotification() {
        if (!isRunning) return;
        Intent svcIntent = new Intent(this, MetronomeService.class);
        svcIntent.putExtra("bpm", bpm);
        svcIntent.putExtra("paused", isPaused);
        svcIntent.putExtra("running", isRunning);
        startService(svcIntent);
    }

    // ===== 来电自动暂停（权限 / 与 Service 状态同步）=====

    /** 惰性申请 READ_PHONE_STATE；已授予时确保 Service 已注册监听。 */
    private void maybeRequestPhonePermission() {
        if (phonePermissionLauncher == null) return;
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.READ_PHONE_STATE)
                == PackageManager.PERMISSION_GRANTED) {
            MetronomeService.onPhonePermissionGranted();
            return;
        }
        if (phonePermissionRequested) return;
        phonePermissionRequested = true;
        phonePermissionLauncher.launch(Manifest.permission.READ_PHONE_STATE);
    }

    /** Android 13+ 通知需 POST_NOTIFICATIONS，否则被抑制且无法回到 App。 */
    private void maybeRequestNotificationPermission() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return;
        if (notificationPermissionLauncher == null) return;
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS)
                == PackageManager.PERMISSION_GRANTED) {
            return;
        }
        if (notificationPermissionRequested) return;
        notificationPermissionRequested = true;
        notificationPermissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS);
    }

    /** 回到前台时读取持久化状态，对齐 UI 与计时器（如通话暂停仍在进行）。 */
    private void syncFromPersistedState() {
        boolean running = MetronomeService.hasPersistedPlayback(this);
        boolean paused = running && MetronomeService.wasPersistedPaused(this);
        syncUiWithServiceState(running, paused);
    }

    /** 将 Service 驱动的运行/暂停状态映射到本页 UI 与计时器；主线程调用。 */
    private void syncUiWithServiceState(boolean running, boolean paused) {
        if (running == isRunning && paused == isPaused) return;
        isRunning = running;
        isPaused = paused;
        // 通知栏 STOP 不会走 stopPlayback()，必须在这里归零计时器
        if (!running) elapsedSeconds = 0;
        timerHandler.removeCallbacks(timerRunnable);
        if (isRunning && !isPaused) {
            timerHandler.postDelayed(timerRunnable, 1000);
            scheduleAutoLock();
        } else {
            cancelAutoLock();
        }
        updateUI();
    }

    // ===== 电池优化引导 =====

    /** 仅在 onCreate 检查一次，避免每次 START 都打扰。 */
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
        updateCadenceUi();
        if (isRunning && !isPaused) {
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        } else {
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        }
    }

    private void resetCadenceStats() {
        final double[] s = CadenceNative.snapshot();
        if (s[CadenceNative.STEPS] > 0) {
            Logx.i(TAG_CADENCE, String.format(Locale.US,
                    "session avg=%.1f max=%.1f acc=%.2f steps=%d sec=%.0f",
                    s[CadenceNative.AVG], s[CadenceNative.MAX], s[CadenceNative.ACCURACY],
                    (long) s[CadenceNative.STEPS], s[CadenceNative.ACTIVE_SEC]));
        }
        CadenceNative.reset();
        lastCadenceSpm = 0.0;
        updateCadenceUi();
    }

    /** 环形仪表与步数；由 1s 计时器与状态变化驱动。 */
    private void updateCadenceUi() {
        if (cadenceRing == null || tvPrimaryValue == null) return;

        final double[] snapshot = CadenceNative.snapshot();
        final double cadence = snapshot[CadenceNative.CADENCE];
        final boolean tracking = snapshot[CadenceNative.TRACKING] != 0;
        if (tracking && cadence > 0) lastCadenceSpm = cadence;

        CadenceRingView.Mode mode;
        if (!isRunning) {
            mode = CadenceRingView.Mode.IDLE;
        } else if (!cadenceSupported) {
            mode = CadenceRingView.Mode.WAITING;
        } else if (isPaused) {
            mode = CadenceRingView.Mode.PAUSED;
        } else if (tracking) {
            mode = CadenceRingView.Mode.TRACKING;
        } else {
            mode = CadenceRingView.Mode.WAITING;
        }

        final int accent = ACCENT_COLORS[accentColorIndex];
        final int secondary = ContextCompat.getColor(this, R.color.textSecondary);
        final String targetText = String.format(Locale.US, "%.1f", bpm);
        final boolean idle = mode == CadenceRingView.Mode.IDLE;
        final boolean paused = mode == CadenceRingView.Mode.PAUSED;

        layoutTargetRow.setVisibility(idle ? View.GONE : View.VISIBLE);
        tvLabelPrimary.setText(idle ? R.string.label_bpm : R.string.label_spm);
        tvTargetValue.setText(targetText);

        if (idle) {
            tvPrimaryValue.setTextSize(TypedValue.COMPLEX_UNIT_SP, isWholeBpm() ? 62f : 40f);
            tvPrimaryValue.setText(idleBpmText());
            tvPrimaryValue.setTextColor(accent);
            tvCadenceStatus.setText(R.string.msg_start_for_cadence);
            tvCadenceStatus.setTextColor(secondary);
        } else {
            final String valueText;
            if (mode == CadenceRingView.Mode.TRACKING) {
                valueText = String.valueOf(Math.round(cadence));
            } else if (paused && lastCadenceSpm > 0) {
                valueText = String.valueOf(Math.round(lastCadenceSpm));
            } else {
                valueText = "–";
            }
            final boolean hasValue = !"–".equals(valueText);
            tvPrimaryValue.setTextSize(TypedValue.COMPLEX_UNIT_SP, hasValue ? 62f : 40f);
            tvPrimaryValue.setText(valueText);
            tvPrimaryValue.setTextColor(!hasValue || paused ? secondary : accent);
            tvTargetValue.setTextColor(paused ? secondary : targetValueColor);

            if (mode == CadenceRingView.Mode.TRACKING) {
                final long delta = Math.round(cadence - bpm);
                tvCadenceStatus.setText(String.format(Locale.US, "Δ %+d", delta));
                tvCadenceStatus.setTextColor(deltaColor(delta));
            } else if (paused) {
                tvCadenceStatus.setText(R.string.label_paused_status);
                tvCadenceStatus.setTextColor(secondary);
            } else if (!cadenceSupported) {
                tvCadenceStatus.setText(R.string.msg_no_sensor);
                tvCadenceStatus.setTextColor(secondary);
            } else {
                tvCadenceStatus.setText(R.string.label_waiting);
                tvCadenceStatus.setTextColor(secondary);
            }
        }

        final float shownCadence = mode == CadenceRingView.Mode.TRACKING
                ? (float) cadence
                : mode == CadenceRingView.Mode.PAUSED ? (float) lastCadenceSpm : 0f;
        cadenceRing.setData(mode, shownCadence, (float) bpm);

        tvSteps.setText(getString(R.string.label_steps_fmt,
                String.format(Locale.getDefault(), "%,d", (long) snapshot[CadenceNative.STEPS])));
    }

    private String signedDelta(double delta) {
        return String.format(Locale.US, "%+d", Math.round(delta));
    }

    private int deltaColor(double delta) {
        final double abs = Math.abs(delta);
        if (abs <= 3.0) return ACCENT_COLORS[accentColorIndex];
        if (abs <= 6.0) return ContextCompat.getColor(this, R.color.cadence_warn);
        return ContextCompat.getColor(this, R.color.cadence_bad);
    }

    private void updateBpmDisplay() {
        tvTargetValue.setText(bpmText());
        if (!isRunning) {
            tvPrimaryValue.setTextSize(TypedValue.COMPLEX_UNIT_SP, isWholeBpm() ? 62f : 40f);
            tvPrimaryValue.setText(idleBpmText());
        }
    }

    private boolean isWholeBpm() {
        return Math.abs(bpm - Math.round(bpm)) < 0.05;
    }

    /** 环心停止态：整数步频用 62sp，带小数时收一档避免顶到数值列边缘。 */
    private String idleBpmText() {
        return isWholeBpm()
                ? String.valueOf(Math.round(bpm))
                : String.format(Locale.US, "%.1f", bpm);
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
        GradientDrawable circleBg = new GradientDrawable();
        circleBg.setShape(GradientDrawable.OVAL);
        circleBg.setColor(color);
        btnThemeColor.setBackground(circleBg);
        boolean isEn = getResources().getConfiguration().getLocales().get(0).getLanguage().equals("en");
        tvThemeName.setText(isEn ? ACCENT_NAMES_EN[accentColorIndex] : ACCENT_NAMES_ZH[accentColorIndex]);
        tvThemeName.setTextColor(color);
        for (Button btn : new Button[]{btnBpmMinus5, btnBpmMinus1, btnBpmPlus1, btnBpmPlus5}) {
            btn.setBackground(makeBpmRippleBg(color));
        }
        btnStartPause.setBackground(makeStartPauseBg(color));
        btnGain.setBackground(makeBpmRippleBg(color));
        btnGain.setTextColor(color);
        ColorStateList csl = ColorStateList.valueOf(color);
        seekTickVolume.setThumbTintList(csl);
        seekTickVolume.setProgressTintList(csl);
        unlockProgress.setProgressTintList(csl);
        tvAccentName.setTextColor(color);
        tvAccentArrow.setTextColor(color);
        tvThemeArrow.setTextColor(color);
        tvLangValue.setTextColor(color);
        drawerLang.setBackground(makeTimbreRowBg(color));
        drawerTheme.setBackground(makeTimbreRowBg(color));
        drawerAccent.setBackground(makeTimbreRowBg(color));
        tvTimbreName.setTextColor(color);
        tvTimbreArrow.setTextColor(color);
        drawerTimbre.setBackground(makeTimbreRowBg(color));
        drawerLogToFile.setBackground(makeTimbreRowBg(color));
        btnCloseSettings.setTextColor(color);
        btnSettings.setColorFilter(color);
        int[][] switchStates = {{android.R.attr.state_checked}, {}};
        switchLogToFile.setThumbTintList(new ColorStateList(switchStates,
                new int[]{color, 0xFF9E9E9E}));
        switchLogToFile.setTrackTintList(new ColorStateList(switchStates,
                new int[]{
                        Color.argb(128, Color.red(color), Color.green(color), Color.blue(color)),
                        0xFF3A3A3A
                }));
        updateLogStateText(switchLogToFile.isChecked());
        targetValueColor = deriveTargetColor(color);
        if (isRunning && isPaused) {
            final int secondary = ContextCompat.getColor(this, R.color.textSecondary);
            tvPrimaryValue.setTextColor(secondary);
            tvTargetValue.setTextColor(secondary);
        } else {
            tvPrimaryValue.setTextColor(color);
            tvTargetValue.setTextColor(targetValueColor);
        }
        btnStats.setTextColor(color);
        cadenceRing.setAccent(color);
        cadenceRing.setTargetColor(targetValueColor);
        btnRestart.setBackground(makeBpmRippleBg(color));
        btnRestart.setTextColor(color);
        if (!restoringUi) Logx.i(TAG_UI, "theme=" + accentColorIndex + " " + tvThemeName.getText());
    }

    /** 目标数字色：沿用主题色相、压饱和，与实测步频区分又保持同族。 */
    private static int deriveTargetColor(int accent) {
        final float[] hsv = new float[3];
        Color.colorToHSV(accent, hsv);
        if (hsv[1] < 0.15f) return 0xFF8E8E93;
        hsv[1] *= 0.62f;
        hsv[2] = Math.min(1f, hsv[2] * 0.96f);
        return Color.HSVToColor(hsv);
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
        String next = prefsManager.getLanguage().equals("zh") ? "en" : "zh";
        Logx.i(TAG_UI, "language=" + next);
        prefsManager.setLanguage(next);
        applyLanguage(next);
        // recreate() 不杀进程，后台服务与 WakeLock 不受影响
        recreate();
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
        btnLock.setContentDescription(getString(isLocked ? R.string.desc_unlock : R.string.desc_lock));
        int accent = ACCENT_COLORS[accentColorIndex];
        // 锁定只禁用会改变播放状态的入口
        for (Button btn : new Button[]{btnBpmMinus5, btnBpmMinus1, btnBpmPlus1, btnBpmPlus5}) {
            btn.setEnabled(enabled);
            btn.setBackground(makeBpmRippleBg(accent));
        }
        layoutCadenceTap.setEnabled(enabled);
        layoutCadenceTap.setAlpha(enabled ? 1.0f : 0.6f);
        seekTickVolume.setEnabled(enabled);
        seekTickVolume.setAlpha(enabled ? 1.0f : 0.6f);
        btnGain.setEnabled(enabled);
        btnGain.setAlpha(enabled ? 1.0f : 0.6f);
        layoutBpmControls.setAlpha(enabled ? 1.0f : 0.6f);
        btnStartPause.setEnabled(enabled);
        btnStartPause.setAlpha(enabled ? 1.0f : 0.6f);
        btnRestart.setEnabled(enabled);
        btnRestart.setAlpha(enabled ? 1.0f : 0.6f);
    }
}
