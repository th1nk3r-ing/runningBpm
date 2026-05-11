# RunBeat Pro — 任务拆解

> 方向：Android App 框架 → Native C++ 核心 → Android UI → iOS UI

---

## Phase 1: Android App 框架 (Hello World)

**目标**：搭建可编译运行的最小 Android 工程（Java + XML），屏幕显示 "Hello RunBeat"。

### 1.1 项目初始化 ✅
- [x] 用 Android Studio 或 CLI 创建 Android 项目骨架
- [x] 配置 Gradle Groovy DSL（`build.gradle`）
  - [x] minSdk = 26，targetSdk = 34
  - [x] 仅依赖 AndroidX AppCompat + Material Design
- [x] 配置 `AndroidManifest.xml`
  - [x] Activity 声明（launcher）
  - [x] `FOREGROUND_SERVICE` + `FOREGROUND_SERVICE_SPECIAL_USE` 权限（提前申领，Phase 3 用）
  - [x] `WAKE_LOCK` 权限
- [x] 创建 `MainActivity.java`
  - [x] `onCreate` → `setContentView(R.layout.activity_main)`
  - [x] 初始化一个 `TextView` 显示 "Hello RunBeat"
- [x] 创建 `res/layout/activity_main.xml`：基础线性布局，居中 `TextView`
- [x] **验收**：`./gradlew assembleDebug` 通过，安装到设备屏幕显示 "Hello RunBeat"

### 1.2 基础骨架搭建 ✅
- [x] 设计 `activity_main.xml` 整体布局框架（用 ViewGroup 占位，Phase 3 填入实际控件）
- [x] 配置深色主题（`res/values/themes.xml`）
  - [x] `Theme.Material3.Dark` + `colorPrimary #FF6B35`
  - [x] 背景色 `#121212`
- [x] 配置 `proguard-rules.pro`（保留 native 方法，避免混淆）
- [x] 创建 `AudioEngine.java`（空壳 JNI 接口类，Phase 2 填方法）

---

## Phase 2: Native C++ 核心引擎

**目标**：实现完整音频引擎，通过 JNI 被 Android 调用，耳机输出精准节拍。

### 2.1 构建体系
- [ ] 在 `app/src/main/cpp/` 下创建目录结构
  ```
  cpp/
    ├── CMakeLists.txt
    ├── core/
    │    ├── Clock.hpp
    │    ├── SamplePlayer.hpp
    │    ├── Mixer.hpp
    │    ├── Limiter.hpp
    │    └── AudioEngine.hpp
    └── oboe_wrapper.cpp
  ```
- [ ] 编写 `CMakeLists.txt`
  - [ ] 编译 `core/` 源码为 `librunbeat_core`
  - [ ] 引入 Oboe 库（源码或 prebuilt .aar）
- [ ] Gradle 配置 `externalNativeBuild { cmake { ... } }`
- [ ] 编写第一个 JNI 探针函数 `Java_com_runbeat_audio_AudioEngine_nativeHello()`
  - [ ] 返回 `jstring`，logcat 打一行 "native loaded"
- [ ] **验收**：`AudioEngine.java` 侧调用 `nativeHello()`，logcat 看到 native 输出

### 2.2 相位累加器 — `Clock.hpp`
- [ ] 数据结构
  - [ ] `double phase_ = 0.0`
  - [ ] `double deltaPhi_ = 0.0`
  - [ ] `int sampleRate_`
- [ ] `SetBPM(double bpm)` → `deltaPhi_ = bpm / (60.0 * sampleRate_)`
- [ ] `Process(int numFrames)` → per-sample 循环
  - [ ] `phase_ += deltaPhi_`
  - [ ] 若 `phase_ >= 1.0`：标记 tick 触发位，`phase_ -= 1.0`
  - [ ] 返回 tick 触发帧索引数组
- [ ] **验收**：构造 BPM=150, sampleRate=48000 输入 4800000 步（100 秒），验证累积相位误差 < 1e-15

### 2.3 采样播放器 — `SamplePlayer.hpp`
- [ ] 数据结构
  - [ ] `std::vector<float> sampleData_`（预加载的 PCM 样本）
  - [ ] `uint32_t readPos_ = 0`
  - [ ] `bool playing_ = false`
- [ ] `Load(const float* data, size_t length)` → 拷贝 PCM 到内部 buffer
- [ ] `Play()` → `readPos_ = 0; playing_ = true`
- [ ] `Render(float* out, int numFrames, float gain)`
  - [ ] 若 `playing_`：逐采样拷贝 `sampleData_[readPos_++] * gain`
  - [ ] 若 `readPos_ >= sampleData_.size()`：输出静音，`playing_ = false`
- [ ] 引擎持有两个实例：`tickPlayer_`、`chimePlayer_`
- [ ] **验收**：Play 后 Render N 帧，输出非零；播放完毕后 Render 输出全零

### 2.4 混音器 — `Mixer.hpp`
- [ ] `Process(float* tickBuf, float* chimeBuf, float* out, int numFrames, float tickGain, float chimeGain)`
  - [ ] 公式：`out[i] = tickBuf[i] * tickGain + chimeBuf[i] * chimeGain`
- [ ] 纯头文件实现，无状态，内联优化

### 2.5 软限幅器 — `Limiter.hpp`
- [ ] `Process(float* buffer, int numFrames)`
  - [ ] `tanh(x)` 或 `clamp(x, -0.95f, 0.95f)`（含 3% headroom）
- [ ] 纯头文件实现

### 2.6 音频引擎状态机 — `AudioEngine.hpp`
- [ ] 状态枚举：`Idle | Running | Paused`
- [ ] 成员变量
  - [ ] `Clock clock_`
  - [ ] `SamplePlayer tickPlayer_`, `chimePlayer_`
  - [ ] `std::atomic<double> bpm_`（lock-free 交换）
  - [ ] `std::atomic<double> tickGain_`, `chimeGain_`
  - [ ] `std::atomic<int64_t> bpmBits_`（如 atomic double 不满足 is_lock_free）
  - [ ] `EngineState state_`
- [ ] `Start(double bpm)` → 状态 → Running，clock.SetBPM(bpm)
- [ ] `Stop()` → 状态 → Idle
- [ ] `Pause()` / `Resume()` → 状态 → Paused / Running，相位保持
- [ ] `OnAudioCallback(float* out, int numFrames)` — 完整 Pipeline：
  ```
  Clock.Process()     → 哪些帧触发 tick
  tickPlayer.Render() → tickBuf[]
  chimePlayer.Render()→ chimeBuf[]
  Mixer.Process()     → mixBuf[]
  Limiter.Process()   → out[]
  ```
- [ ] **严禁回调内**：mutex、printf/log、malloc/new、系统调用
- [ ] **验收**：模拟回调循环输出 10s 音频数据，写入 WAV 文件，audacity 查看节拍间隔均等

### 2.7 JNI 桥接 & Oboe 驱动 — `oboe_wrapper.cpp`
- [ ] Native 函数对外暴露
  - [ ] `nativeStart(jdouble bpm)`
  - [ ] `nativeStop()`
  - [ ] `nativePause()`
  - [ ] `nativeResume()`
  - [ ] `nativeSetBpm(jdouble bpm)`
  - [ ] `nativeSetTickVolume(jdouble vol)`
  - [ ] `nativeSetChimeVolume(jdouble vol)`
  - [ ] `nativeTriggerChime()`（整点报时入口）
- [ ] 全局（或 static）`AudioEngine engine_` 实例
- [ ] Oboe `AudioStreamBuilder` 配置
  - [ ] `PerformanceMode::LowLatency`
  - [ ] `SharingMode::Exclusive`
  - [ ] `Format::Float`
  - [ ] `SampleRate::48000`
- [ ] 降级逻辑：独占失败 → 自动回退 `SharingMode::Shared`
- [ ] `AudioStreamCallback::onAudioReady()` → `engine_.OnAudioCallback()`
- [ ] XRun 监控：回调内计数 overflow/underrun，log 上报
- [ ] **验收**：`AudioEngine.java` 调用 `nativeStart(180.0)`，耳机/扬声器听到 180BPM 节拍声

### 2.8 WAV 资源准备
- [ ] 准备 `tick_hi.wav` / `tick_lo.wav`（强拍弱拍）和 `chime.wav`
- [ ] 或用代码生成正弦/指数衰减脉冲（无外部依赖）
- [ ] 在 JNI 初始化时加载并重采样至硬件采样率
- [ ] 放入 `res/raw/` 或 `assets/`

---

## Phase 3: Android UI

**目标**：用 Java + XML 实现完整跑步节拍器 UI，通过 JNI 驱动 Native 引擎。

### 3.1 主界面布局 — `activity_main.xml`
按设计稿自上而下排列：

```
┌──────────────────────────────────┐
│  ● RUNNING                   🔒  │  状态栏 (LinearLayout horizontal)
├──────────────────────────────────┤
│           00:32:18               │  计时区 (TextView, monospace)
│                                  │
│              180                 │  BPM 核心区 (TextView, 超大号)
│              BPM                 │
├──────────────────────────────────┤
│    [-5]   [-1]   [+1]   [+5]    │  调节区 (4 × Button)
├──────────────────────────────────┤
│        START / PAUSE             │  控制区 (Button, 品牌色)
├──────────────────────────────────┤
│ Tick Vol ▓▓▓▓░░  Accent [ON]    │  参数区 (SeekBar + Switch)
└──────────────────────────────────┘
```

- [ ] **状态区**（`LinearLayout` 横向）
  - `ImageView` 运行指示灯（绿/灰圆点 drawable）
  - `TextView` 显示 "RUNNING" / "PAUSED"
  - `Switch` 锁定开关（`android:id="@+id/switchLock"`）
- [ ] **计时区**
  - `TextView`（`android:id="@+id/tvTimer"`）
  - `android:typeface="monospace"`，大号字体，防止字符跳动
- [ ] **BPM 核心区**
  - `TextView` 大号数字（`android:id="@+id/tvBpm"`）
  - `TextView` 标签 "BPM"（`android:id="@+id/tvBpmLabel"`）
- [ ] **调节区**（`LinearLayout` 横向居中）
  - 四个 `Button`：`[-5]` `[-1]` `[+1]` `[+5]`
  - `android:minWidth="44dp"` `android:minHeight="44dp"` 最小触摸热区
  - `android:background` 圆角矩形 drawable
- [ ] **控制区**
  - `Button` `START` / `PAUSE`（`android:id="@+id/btnStartPause"`）
  - 品牌色背景 `#FF6B35`
- [ ] **参数区**
  - `SeekBar`（`android:id="@+id/sbTickVolume"`，范围 0-100）
  - `Switch` Accent 开关（`android:id="@+id/switchAccent"`）

### 3.2 Activity 状态管理 — `MainActivity.java`
- [ ] 成员变量
  - `double bpm`（默认 180.0）
  - `boolean isRunning`
  - `long elapsedSeconds`
  - `float tickVolume`（0.0f - 1.0f）
  - `boolean accentOn`
  - `boolean isLocked`
- [ ] `onCreate()` 生命周期
  - [ ] `setContentView(R.layout.activity_main)`
  - [ ] `findViewById` 绑定所有控件
  - [ ] 注册按钮 `OnClickListener`
  - [ ] 加载 native library（`System.loadLibrary("runbeat")`）
  - [ ] 从 `savedInstanceState` 恢复状态
- [ ] `updateUI()` 方法：根据当前状态刷新所有控件显示
- [ ] `onSaveInstanceState()`：持久化 BPM / 计时等关键状态

### 3.3 UI 交互逻辑
- [ ] **BPM 调节**
  - [ ] 点击 `[-5]` `[-1]` `[+1]` `[+5]` → 调用 `AudioEngine.nativeSetBpm(newBpm)`
  - [ ] 长按 `[-5]` / `[+5]`：`OnLongClickListener` + `Handler.postDelayed` 连续调节（每 100ms）
  - [ ] 调节后更新 `tvBpm` 文本
- [ ] **START / PAUSE**
  - [ ] 点击 → 切换 `isRunning` 状态
  - [ ] `isRunning = true`：调用 `AudioEngine.nativeStart(bpm)`，按钮文字变为 "PAUSE"，启动计时 Handler
  - [ ] `isRunning = false`：调用 `AudioEngine.nativeStop()`，按钮文字变为 "START"，暂停计时
- [ ] **计时器**
  - [ ] `Handler` + `Runnable`，每 1s 刷新 `tvTimer`
  - [ ] 格式化 `HH:mm:ss`
- [ ] **音量 & Accent**
  - [ ] `SeekBar.OnSeekBarChangeListener` → 调用 `AudioEngine.nativeSetTickVolume(vol / 100f)`
  - [ ] `Switch.OnCheckedChangeListener` → 调用 `AudioEngine.nativeSetAccent(on)`
- [ ] **锁定逻辑**
  - [ ] `switchLock.setOnCheckedChangeListener` → 设置 `isLocked`
  - [ ] `isLocked = true` 时：`setEnabled(false)` BPM 按钮 + SeekBar + Accent Switch
  - [ ] START / PAUSE 保持可用
  - [ ] 解锁：长按锁定图标 → Toast 提示 → 解除锁定

### 3.4 JNI 接口类 — `AudioEngine.java`
`package com.runbeat.audio;`
- [ ] `static { System.loadLibrary("runbeat"); }`
- [ ] Native 方法声明
  - `public static native void nativeStart(double bpm);`
  - `public static native void nativeStop();`
  - `public static native void nativePause();`
  - `public static native void nativeResume();`
  - `public static native void nativeSetBpm(double bpm);`
  - `public static native void nativeSetTickVolume(double vol);`
  - `public static native void nativeSetChimeVolume(double vol);`
  - `public static native void nativeSetAccent(boolean on);`
  - `public static native void nativeTriggerChime();`
- [ ] 音频回调 → Java 进度更新（通过 `JNIEnv->CallStaticVoidMethod` 回调静态方法）

### 3.5 保活 & 前台服务 — `MetronomeService.java`
- [ ] 继承 `Service`，声明为 `Foreground Service`
  - [ ] 持有 Native Engine 实例（Activity 销毁不影响）
  - [ ] 创建通知渠道（`NotificationChannel`，targetSdk 34 要求）
  - [ ] 通知栏常驻："RunBeat · 180 BPM · RUNNING"
  - [ ] 通知栏添加 `PendingIntent`：PAUSE / STOP 快捷操作
- [ ] `WakeLock`（`PARTIAL_WAKE_LOCK`）
  - [ ] `PowerManager.WakeLock` 运行时 acquire，保证 CPU 不休眠
  - [ ] onDestroy / Stop 时 release
- [ ] 屏幕常亮
  - [ ] `getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)`
- [ ] 电池优化检测
  - [ ] `isIgnoringBatteryOptimizations()` → false 时弹出 `AlertDialog` 引导
  - [ ] 点击跳转 `Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS`
- [ ] Activity 与 Service 通信
  - [ ] `bindService()` + `ServiceConnection`：获取状态
  - [ ] 前后台切换：Service 持续运行，onResume 时读取最新 BPM / 计时

### 3.6 触觉 & 交互增强
- [ ] BPM 按钮震动反馈
  - `view.performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK)`
  - API 31+ 用 `VibratorManager`，低版本用 `Vibrator`
- [ ] 锁定图标长按 2s 解锁（`OnLongClickListener` + `Handler.postDelayed`）
- [ ] BPM 数字更新时简单缩放动画（`ValueAnimator`）

---

## Phase 4: iOS UI

**目标**：iOS 端实现相同功能和 UI，直接复用 C++ Core 源码。

### 4.1 iOS 项目初始化
- [ ] 创建 Xcode 项目（SwiftUI App）
- [ ] 配置 C++ 源码引用
  - [ ] 将 `core/` 下所有 `.hpp` 文件加入 Xcode target
  - [ ] 设置 `C++ Language Dialect = C++17`
- [ ] 配置 Audio Session
  - [ ] `AVAudioSession.sharedInstance().setCategory(.playback, options: [.mixWithOthers])`
  - [ ] `try session.setActive(true)`
- [ ] **验收**：SwiftUI 界面显示 "Hello RunBeat"

### 4.2 AudioUnit 驱动
- [ ] 实现 `AudioEngineBridge.mm`
  - [ ] 持有 `AudioEngine` C++ 实例
  - [ ] 创建 RemoteIO AudioUnit
  - [ ] 设置 `kAudioUnitProperty_StreamFormat`（48kHz Float32）
  - [ ] AudioUnit 回调 → `engine.onAudioCallback()`
- [ ] Swift 接口包装
  - [ ] `start(bpm:)`, `stop()`, `pause()`, `resume()`, `setBpm(_:)`, `setVolume(_:)`
- [ ] **验收**：Swift 调用 `start(180)`，耳机听到节拍

### 4.3 SwiftUI 主界面
- [ ] 界面布局与 Android 一致（见 Phase 3.1 设计稿）
  - [ ] 状态区：Circle + Text + Toggle
  - [ ] 计时区：`.font(.system(.title, design: .monospaced))`
  - [ ] BPM 核心区：大号字体
  - [ ] 调节区：`[-5] [-1] [+1] [+5]` 按钮 + `UIImpactFeedbackGenerator`
  - [ ] START / PAUSE 按钮
  - [ ] 音量 / Accent 参数区
- [ ] 状态管理：`@StateObject MainViewModel` 或 `@Observable`
- [ ] 锁定模式逻辑与 Android 一致

### 4.4 锁屏 & 后台
- [ ] `MPRemoteCommandCenter` 注册
  - [ ] `.play` / `.pause` / `.togglePlayPause`
  - [ ] 自定义 BPM+ / BPM- command
- [ ] `MPNowPlayingInfoCenter`
  - [ ] 展示标题 "RunBeat" + 当前 BPM + 已用时间
- [ ] 后台音频持续运行（AVAudioSession 已配）

---

## 里程碑

| # | 阶段 | 可交付物 | 依赖 |
| --- | --- | --- | --- |
| M1 | Phase 1 完成 | Android 工程编译通过，屏幕显示 Hello RunBeat | 无 |
| M2 | Phase 2 完成 | 耳机输出 180BPM 节拍声，JNI 可调 BPM/音量 | M1 |
| M3 | Phase 3 完成 | 完整 Android App（UI + 保活 + Service），可在跑步中使用 | M2 |
| M4 | Phase 4 完成 | iOS App 功能与 Android 一致，复用 Core 代码 | M2 |
