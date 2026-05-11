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

### 2.1 构建体系 ✅
- [x] 在 `app/src/main/cpp/` 下创建目录结构
  ```
  cpp/
    ├── CMakeLists.txt
    ├── core/
    │    ├── Clock.hpp
    │    ├── SamplePlayer.hpp
    │    ├── Mixer.hpp
    │    ├── Limiter.hpp
    │    └── AudioEngine.hpp
    ├── aaudio_engine.cpp
  ```
- [x] 编写 `CMakeLists.txt`
  - [x] 编译 `core/` 源码为 `librunbeat_core`
  - [x] 链接 AAudio 共享库（`libaaudio.so`，NDK 自带）
- [x] Gradle 配置 `externalNativeBuild { cmake { ... } }`
- [x] 编写第一个 JNI 探针函数 `Java_com_runbeat_audio_AudioEngine_nativeHello()`
  - [x] 返回 `jstring`，logcat 打一行 "native loaded"
- [x] **验收**：`./gradlew assembleDebug` 通过，native 代码编译链接成功

### 2.2 相位累加器 — `Clock.hpp` ✅
- [x] 数据结构
  - [x] `double framesPerTick_`（等价于 phase + deltaPhi 模型）
  - [x] `double framesToNextTick_`（倒计数替代累加，误差不跨 tick 累积）
  - [x] `int sampleRate_`
- [x] `SetBPM(double bpm)` → `framesPerTick_ = sampleRate * 60 / bpm`
- [x] `Process(int numFrames)` → per-sample 循环
  - [x] `framesToNextTick_ -= 1.0`
  - [x] 若 `<= 0.0`：标记 tick 触发位，`framesToNextTick_ += framesPerTick_`
  - [x] 返回 tick 触发帧索引数组
- [x] **验收**：BPM=150, sampleRate=48000, 4800000 步（100 秒）→ 250 ticks，零累积误差

### 2.3 采样播放器 — `SamplePlayer.hpp` ✅
- [x] 数据结构
  - [x] `std::vector<float> sampleData_`（预加载的 PCM 样本）
  - [x] `uint32_t readPos_ = 0`
  - [x] `bool playing_ = false`
- [x] `Load(const float* data, size_t length)` → 拷贝 PCM 到内部 buffer
- [x] `Play()` → `readPos_ = 0; playing_ = true`
- [x] `Render(float* out, int numFrames, float gain)`
  - [x] 若 `playing_`：逐采样拷贝 `sampleData_[readPos_++] * gain`
  - [x] 若 `readPos_ >= sampleData_.size()`：输出静音，`playing_ = false`
- [x] 引擎持有两个实例：`tickPlayer_`、`chimePlayer_`
- [x] **验收**：Play 后 Render N 帧，输出非零；播放完毕后 Render 输出全零

### 2.4 混音器 — `Mixer.hpp` ✅
- [x] `Process(float* tickBuf, float* chimeBuf, float* out, int numFrames, float tickGain, float chimeGain)`
  - [x] 公式：`out[i] = tickBuf[i] * tickGain + chimeBuf[i] * chimeGain`
- [x] 纯头文件实现，无状态，内联优化

### 2.5 软限幅器 — `Limiter.hpp` ✅
- [x] `Process(float* buffer, int numFrames)`
  - [x] `tanh(x)` 平滑软限幅，保留 3% headroom
  - [x] `ProcessHard` 硬限幅备选（clamp ±0.97）
- [x] 纯头文件实现

### 2.6 音频引擎状态机 — `AudioEngine.hpp` ✅
- [x] 状态枚举：`Idle | Running | Paused`
- [x] 成员变量
  - [x] `Clock clock_`
  - [x] `SamplePlayer tickPlayer_`, `chimePlayer_`
  - [x] `std::atomic<double> bpm_`（lock-free 交换）
  - [x] `std::atomic<double> tickGain_`, `chimeGain_`
  - [x] `std::atomic<State> state_`
- [x] `Start(double bpm)` → 状态 → Running
- [x] `Stop()` → 状态 → Idle，重置 Clock + Player
- [x] `Pause()` / `Resume()` → 状态切换，相位保持
- [x] `OnAudioCallback(float* out, int numFrames)` — 完整 Pipeline：
  ```
  per-sample: Clock.Advance() → 触发 Play()
              tickPlayer.ReadOne() × gain
              chimePlayer.ReadOne() × gain
              → 混合 → Limiter::Process(out, numFrames)
  ```
- [x] **严禁回调内**：mutex、printf/log、malloc/new、系统调用
- [x] **验收**：模拟回调循环输出 10s 音频，WAV 文件节拍间隔 19200.00 帧，零抖动

### 2.7 JNI 桥接 & AAudio 驱动 — `aaudio_engine.cpp` ✅
- [x] Native 函数对外暴露
  - [x] `nativeInit()` / `nativeDestroy()`
  - [x] `nativeStart(jdouble bpm)`
  - [x] `nativeStop()`
  - [x] `nativePause()` / `nativeResume()`
  - [x] `nativeSetBpm(jdouble bpm)`
  - [x] `nativeSetTickVolume(jdouble vol)` / `nativeSetChimeVolume(jdouble vol)`
  - [x] `nativeSetAccent(jboolean on)`
  - [x] `nativeTriggerChime()`
  - [x] `nativeGetXRunCount()`
- [x] 全局 `static AudioEngine gEngine` 实例
- [x] AAudio 流构建（`AAudioStreamBuilder`）
  - [x] `AAUDIO_PERFORMANCE_MODE_LOW_LATENCY`
  - [x] `AAUDIO_SHARING_MODE_EXCLUSIVE` → `SHARED` 降级
  - [x] `AAUDIO_FORMAT_PCM_FLOAT`
  - [x] Data/Error Callback 注册
- [x] 降级逻辑：EXCLUSIVE 失败 → SHARED
- [x] Data Callback → `gEngine.OnAudioCallback()`
- [x] Error Callback：流断开日志记录（Phase 3 实现自动重建）
- [x] XRun 监控：`nativeGetXRunCount()` JNI 函数
- [x] 采样生成：`nativeInit()` 内建 880Hz 指数衰减脉冲
- [x] **验收**：`./gradlew assembleDebug` 通过，native 编译链接成功

### 2.8 WAV 资源准备 ✅
- [x] 生成 `tick_hi.wav`（1000Hz 强拍衰减脉冲）
- [x] 生成 `tick_lo.wav`（600Hz 弱拍衰减脉冲）
- [x] 生成 `chime.wav`（660+880Hz 双音较长延续）
- [x] 放入 `res/raw/`
- [x] WavLoader.hpp 解析 16-bit PCM WAV（含重采样）
- [x] nativeLoadWavAssets() JNI 函数加载 WAV 至引擎

---

## Phase 3: Android UI

**目标**：用 Java + XML 实现完整跑步节拍器 UI，通过 JNI 驱动 Native 引擎。

### 3.1 主界面布局 — `activity_main.xml` ✅
（Phase 1.2 已完成布局框架）

### 3.2 Activity 状态管理 — `MainActivity.java` ✅
- [x] 成员变量
  - `double bpm`（默认 180.0）
  - `boolean isRunning` / `isPaused`
  - `int elapsedSeconds`
  - `float tickVolume`（0.0f - 1.0f）
  - `boolean accentOn` / `isLocked`
- [x] `onCreate()` → `setContentView` + 绑定控件 + 注册监听 + `nativeInit` + WAV 加载
- [x] `updateUI()` → 刷新 BPM/计时/状态/锁定
- [x] `onSaveInstanceState()` → 持久化关键状态

### 3.3 UI 交互逻辑 ✅
- [x] **BPM 调节** → `nativeSetBpm` + 长按 +5/-5 连续调节（100ms interval）
- [x] **START/PAUSE/RESUME** → `nativeStart/Stop/Pause/Resume` + 计时 Handler
- [x] **计时器** → Handler + Runnable，1s 刷新 `HH:mm:ss`
- [x] **音量 & Accent** → SeekBar → `nativeSetTickVolume` / Switch → `nativeSetAccent`
- [x] **锁定逻辑** → Switch 控制 isLocked，禁用 BPM/音量/Accent 控件，START 保持可用

### 3.4 JNI 接口类 — `AudioEngine.java` ✅
（Phase 2 已完成全部 native 方法声明，Phase 3 补充 `nativeInit`/`nativeDestroy`/`nativeLoadWavAssets`/`nativeGetXRunCount`）

### 3.5 保活 & 前台服务 — `MetronomeService.java` ✅
- [x] Foreground Service + 通知渠道（targetSdk 34）
- [x] `PARTIAL_WAKE_LOCK`（acquire/release）
- [x] Activity `keepScreenOn` 标志
- [x] Manifest 注册 service + 权限声明

### 3.6 触觉 & 交互增强 ✅
- [x] BPM 按钮震动反馈
  - `view.performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP)`
- [x] 锁定图标长按 2s 解锁（`OnTouchListener` 拦截 + `Handler.postDelayed`）
- [x] BPM 数字更新时简单缩放动画（`ValueAnimator`）

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
