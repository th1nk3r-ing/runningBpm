# 🏃 专业级跑步节拍器 (RunBeat Pro) 设计方案

### 1. 核心架构：跨平台 "Core + Shell"

将音频运算与系统驱动分离。核心逻辑由 C++ 编写，确保在 Android (AAudio) 和 iOS (AudioUnit) 上算法行为完全一致。

* **Shared Core (C++17)**：处理相位计算、采样生成、混音、限幅。
* **Platform Shell**：处理系统级权限、音频流开启（Exclusive Mode 降级逻辑）、保活策略。

---

### 2. 音频引擎设计：相位累加器 (Phase Accumulator)

为消除跨回调边界的非整数采样点累积误差，引入相位累加模型。与每个 callback 内重置帧计数不同，本方案维护一个跨 buffer 的连续相位，确保节拍触发永远落在正确的绝对采样点上。

* **原理**：定义一个 `double` 类型的相位 $phase \in [0.0, 1.0)$。
* **增量公式**：每一个采样点步进的相位量 $\Delta\phi$ 为：

$$\Delta\phi = \frac{BPM}{60.0 \cdot sampleRate}$$

* **触发逻辑**：在 Buffer 的 `per-sample` 循环中累加。当 $phase \ge 1.0$ 时，触发 Tick 事件，并执行 $phase -= 1.0$。节拍声由事件驱动开始播放，播放完毕后对应声道的输出静音。

---

### 3. 实时安全 (Real-time Safety) 模块

音频回调线程（Audio Thread）具有极高的优先级，任何阻塞都会导致爆音（Glitch）。

* **Lock-free 参数交换**：
  * 使用 `std::atomic<double>` 传递 BPM 和音量系数（实际工程中若平台不支持浮点原子无锁，可替换为 `atomic<int64_t>` 传递位模式）。
  * 严禁在回调中使用 `mutex`、`printf` 或任何内存分配（`new`/`malloc`）。
  * **唯一例外**：每拍观测打点 `RunBeat-Beat`（见 §11.4）。它是验证打点精度的量测工具而非业务日志，约束条件被单独收紧，不得作为其它日志进入回调的先例。

* **双缓冲参数状态 (Double Buffering)**：对于复杂的设置变更，使用双缓冲结构，确保采样期间参数的原子性一致。
* **抗抖动渲染**：逻辑必须封装在 `for (int i = 0; i < numFrames; i++)` 循环内，彻底杜绝回调边界抖动（Callback Boundary Jitter）。

---

### 4. 音频处理链路 (Audio Pipeline)

```text
[相位事件触发] ──→ [Tick 采样播放器] ──→ [Gain] ──╮
                                                    ├──→ [内联混音] ──→ [硬限幅 ±0.97] ──→ [Output]
[事件触发] ────→ [Chime 采样播放器] ──→ [Gain] ──╯
```

* **事件驱动播放**：Tick 由相位累加器触发，Chime（整点报时等）由外部异步事件触发。每个播放器维护独立的读指针，播放完毕后输出静音。
* **内联混音**：在 `AudioEngine::OnAudioCallback` 的逐帧循环内混音（引擎不经过独立的 `Mixer` 组件，`Mixer.hpp` 仅供测试使用）。公式：$S_{out} = (S_{tick} \cdot G_{m}) + (S_{chime} \cdot G_{c})$。
* **硬限幅**：`Limiter::ProcessHard` 在输出前将峰值 clamp 到 ±0.97，保留 click 瞬态（`Limiter::Process` tanh 软限幅仅测试使用）。
* **资源预处理**：音频素材（WAV）在加载阶段即重采样至硬件原生采样率（如 48kHz），避免实时重采样损耗。

---

### 5. 界面与交互设计 (High-Contrast Style)

UI 采用高对比度深色模式，针对跑步时的身体晃动，点击区域做了加宽处理。

#### 5.1 视觉布局图示

```Plaintext
┌──────────────────────────────────┐
│  ● RUNNING (后台运行中)   🔒      │ ← 状态区：大图标显示，防误触锁定开关
├──────────────────────────────────┤
│                                  │
│           00:32:18               │ ← 计时区：Monospace 字体，防止字符跳动
│                                  │
│               180                │ ← 核心区：全屏最显眼位置
│               BPM                │
│                                  │
├──────────────────────────────────┤
│    [-5]   [-1]   [+1]   [+5]     │ ← 调节区：44dp+ 点击热区，带震动反馈
├──────────────────────────────────┤
│                                  │
│          START / PAUSE           │ ← 控制区：醒目品牌色按钮
│                                  │
├──────────────────────────────────┤
│ Tick Vol: 80%  |  Accent: ON     │ ← 参数区：实时反映底层渲染器状态
└──────────────────────────────────┘
```

#### 5.2 交互逻辑细节

触觉反馈 (Haptics)：在点击 BPM 调节按钮时，Android 调用 View.performHapticFeedback，iOS 调用 UIImpactFeedbackGenerator，给跑者确认感。

常亮模式：运行期间请求 keepScreenOn，防止屏幕自动休眠。

锁定逻辑：开启锁定后，屏蔽 BPM 调节、音量、倍率、重音、音色等参数控件，防止跑步时汗水或误触误操作。

---

### 6. Android 平台深度优化 (Anti-Kill & Low-Latency)

Android 系统的碎片化和严苛的后台管理是最大挑战。

* **流管理与降级**：
  * 首选 `AAUDIO_PERFORMANCE_MODE_LOW_LATENCY` + `AAUDIO_SHARING_MODE_EXCLUSIVE`。
  * **降级逻辑**：若独占模式失败（常见于蓝牙耳机或国产 ROM 限制），自动回退至 `AAUDIO_SHARING_MODE_SHARED` 模式。

* **XRun 监控**：累计记录采样溢出（Underrun/Overrun）用于观测；Buffer 尺寸固定为 2×burst。
* **保活三剑客**：
  1. **Foreground Service**：绑定常驻通知栏。
  2. **WakeLock**：防止 CPU 进入深度休眠。
  3. **Battery Optimization**：引导用户在系统设置中为 App 开启"不限制后台活动"。

---

### 7. iOS 平台深度优化 (System Integration)

* **Audio Session**：配置为 `AVAudioSessionCategoryPlayback`，支持后台持续发声。
* **RemoteIO**：直接控制 `AudioUnit` 回调，绕过高层 API 以获得极致稳定性。
* **锁屏集成**：对接 `MPRemoteCommandCenter`，支持在锁屏/耳机线控启停（暂不支持微调 BPM）。

---

### 8. 功能性模块设计

* **整点报时**：
  * 使用系统级精确闹钟（Android `ExactAlarm` / iOS `UNNotification`）触发信号。
  * 信号通过 **SPSC Queue** 异步推送到音频线程。

* **音频路由监测**：
  * 实时监听蓝牙耳机插拔。
  * 处理 `Sample Rate` 变更（如从 48kHz 切换到蓝牙 44.1kHz），自动重建相位增量系数。

---

### 9. 关键性能指标预测

| 指标 | 目标要求 | 实现手段 |
| --- | --- | --- |
| **算法层漂移** | 0 采样/小时（无累积量化误差） | 相位累加器（Double Precision） |
| **绝对时间精度** | 仅受硬件晶振限制（典型 ~20 ppm，月误差秒级） | 跟随音频时钟，跑步场景完全可接受 |
| **BPM 范围** | 150 - 190 (支持 0.1 精度) | 逐采样计算逻辑 |
| **CPU 负载** | < 1% | NDK 原生运算 |
| **后台稳定性** | 连续运行 4h+ 不中断 | 核心服务常驻 + 电源白名单 |

---

### 10. 项目代码结构（实际）

```text
/cpp
  ├── CMakeLists.txt
  ├── aaudio_engine.cpp          // JNI 桥接 + AAudio 驱动
  ├── gen_wav.cpp                // WAV 资源离线生成工具
  ├── /third_party/spdlog        // 日志库（v1.17.0，header-only）
  └── /core (C++ Shared)
        ├── Clock.hpp            // 倒计数式节拍时钟
        ├── SamplePlayer.hpp     // Voice Pool 多复音播放器
        ├── Mixer.hpp            // 双路信号混音（双源→mono）
        ├── Limiter.hpp          // 软/硬限幅器
        ├── AudioEngine.hpp      // 引擎状态机与完整 Pipeline
        ├── Log.hpp              // 统一日志门面（三端共用级别/格式/sink）
        └── WavLoader.hpp        // WAV 解析 + 重采样 + 生成
/android
  └── /app/src/main
        ├── /java/com/runbeat/pro
        │     ├── MainActivity.java      // UI 与交互逻辑
        │     ├── MetronomeService.java  // 前台服务与保活（WakeLock）
        │     └── PreferencesManager.java
        ├── /java/com/runbeat/audio
        │     └── AudioEngine.java       // JNI 接口声明
        └── /assets/sounds/default
              ├── tick_hi.wav
              ├── tick_lo.wav
              └── chime.wav
/ios
  ├── project.yml                         // XcodeGen 工程配置及 WAV 资源同步
  └── RunBeatPro
        ├── App/                           // SwiftUI 入口、AudioSession 配置
        ├── Bridge/AudioEngineBridge.mm    // Obj-C++、共享 C++ Core、RemoteIO
        ├── Model/AppState.swift           // 应用状态与音频控制
        ├── Service/                       // 偏好、触觉、锁屏命令
        └── View/                          // SwiftUI 界面组件
```

---

### 11. 日志系统

统一门面：一套级别、一套格式、一套 sink，C++ / Android / iOS 共用。实现为 `cpp/core/Log.hpp`（header-only），底座是 vendored 的 spdlog v1.17.0。

#### 11.1 分层

| 层 | 入口 | 说明 |
| --- | --- | --- |
| C++ | `beat::log::Get("RunBeat-Native")->info(...)` | 直连 spdlog logger |
| Android | `Logx.i("RunBeat-XRun", ...)` | native 可用时经 JNI 转发，否则回落 `android.util.Log` |
| iOS | `AudioEngineBridge.logInfo("AudioSession", message: ...)` | 同一 `beat::log::Get()`，ObjC 与 Swift 共用 |

除 `RunBeat-Beat` 外都是同步 logger：日志量小、不碰音频线程，因此不留异步队列，开关即时生效。

#### 11.2 级别与 tag

* 级别：`trace/debug/info/warn/error/critical`，全局默认 `trace`，release 同样保留。门面入口：C++ 直连 `->trace/debug/info/warn/error/critical`；Android 为 `Logx.t/d/i/w/e`（`critical` 只有常量与 native 通道，无封装方法）；iOS 为 `logDebug/logInfo/logWarn/logError`。
* 级别口径：`error` 给「功能不可用」（样本加载失败、流 / AudioUnit 创建失败）；`warn` 给「降级但仍可用」（流断开待重建、EXCLUSIVE 回落 SHARED、会话被重置、启动/恢复失败后可重试）；`info` 给状态迁移与周期性观测（`RunBeat-Stats`、`RunBeat-XRun`）；`trace` 给高频数值（每拍、`nativeSetBpm`）；`debug` 目前未使用，留作后续细分。
* tag 沿用原 logcat 标签：`RunBeat-Native`、`RunBeat-Beat`、`RunBeat-Stats`、`RunBeat-XRun`、`RunBeat-Service`、`AudioEngineBridge`、`AudioUnit`、`AudioSession`、`RemoteCommand`。
* `AudioEngine` 不是门面 tag：它只用于 `AudioEngine.java` 里 `librunbeat.so` 加载失败时的 `android.util.Log` 兜底。
* 用户操作统一走 `RunBeat-UI`：一次操作一条 `info`（`start/pause/resume/restart`、Android 额外的 `stop`、`bpm`、`volume`、`gain`、`accent`、`timbre`、`theme`、`language`、`lock`/`unlock`/`auto-lock`、`file log on/off`、权限结果），设置类用「值 + 名称」，失败分支走 `warn`。连续输入（BPM 长按、音量滑块）只在停手且值真的变化时记一条，不逐帧打点。

#### 11.3 sink

| 平台 | 系统日志 | 文件 |
| --- | --- | --- |
| Android | `android_sink`（tag = logger 名，只送原始 message） | `<filesDir>/logs/runbeat.log` |
| iOS | 自绘 `os_log` sink（subsystem `com.runbeat.pro`，category = logger 名） | `Library/Caches/Logs/runbeat.log` |
| host 测试 | 无系统日志落点（Linux 为 null sink，macOS 走 os_log） | 不写 |

iOS 级别映射：`trace/debug → OS_LOG_TYPE_DEBUG`，`info/warn → OS_LOG_TYPE_DEFAULT`（`info` 用 DEFAULT 是为了让 `log show` 事后可查），`err → ERROR`，`critical → FAULT`。

文件按 4 MiB × 2 轮转，3 秒周期 flush；目录不可写时自动降级为只打系统日志。按 180 BPM 约 1.4 MiB/h 估算，单文件约 3 小时、总保留约 6 小时。

文件 sink 挂在 spdlog 自带的 `dist_sink` 上：面板「日志写入文件」开关就是挂载/摘除子 sink。关闭后不再写入、也不新建文件（句柄保留到 `Shutdown`）；`Shutdown` 之后再打开开关不会重开文件，只有下一次 `Init` 才会按新目录建。打开时立即创建文件，所以「开着但还没写」会看到一个 0 字节的 `runbeat.log`。状态存 Android `SharedPreferences` 的 `log_to_file` / iOS `UserDefaults` 同名键，默认开启。

#### 11.4 回调线程内的每拍打点

`RunBeat-Beat` 是全链路唯一跑在音频回调里的日志，用途是核对「目标 BPM 是否精确打点」：逐拍记录 `beatSeq`、绝对帧位置与采样率，事后离线比对漂移。它是量测工具，不是业务日志，因此**刻意**留在回调线程内，中途不经过任何会阻塞的队列。

约束被单独收紧：

* 同步 logger + 两个 sink（logcat + 入队）+ `null_mutex`，正常路径不取锁；`Init` 时缓存裸指针，不做 registry 查找。
* `SPDLOG_ANDROID_RETRIES=0`，`EAGAIN` 不重试也不 sleep 等待 logd。
* 单独装了空的 error handler：logd 写失败直接丢弃，不退到默认 handler 的静态锁 + `localtime` + `fprintf`。
* 落盘不走 `gate`：格式化后的行写入 256 槽的单生产者环形队列（静态内存、无分配、无锁，满即丢），由 `Init` 起的 drain 线程每 100 ms 抽干后经 `gate` 写文件；文件开关同样生效。队列满时把丢弃计数以 `warning` 同时写进系统日志和文件，缺口在两条通道上都可见。
* 记录时间取回调侧的 `msg.time`，文件行的时间戳即打点时刻，落盘延迟不影响漂移分析；与其它 tag 的行序可能有 ≤100 ms 的错位。
* `memory_buf_t` 为 250 字节栈内缓冲，该行（约 30 字符）正常路径不产生堆分配。
* 剩余开销即一次 logd 写（含 `android_sink` 内部一次 250B 栈缓冲拷贝）+ 一次入队拷贝，180 BPM 下每秒 3 次，相对 buffer 周期可忽略。

这条链路的准入条件只有一条：观测打点本身。任何业务日志一律不得借道进入音频回调。
