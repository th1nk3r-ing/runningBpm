# 🏃 专业级跑步节拍器 (RunBeat Pro) 技术方案

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

* **双缓冲参数状态 (Double Buffering)**：对于复杂的设置变更，使用双缓冲结构，确保采样期间参数的原子性一致。
* **抗抖动渲染**：逻辑必须封装在 `for (int i = 0; i < numFrames; i++)` 循环内，彻底杜绝回调边界抖动（Callback Boundary Jitter）。

---

### 4. 音频处理链路 (Audio Pipeline)

```text
[相位事件触发] ──→ [Tick 采样播放器] ──→ [Gain] ──╮
                                                    ├──→ [Mixer] ──→ [Soft Limiter] ──→ [Output]
[事件触发] ────→ [Chime 采样播放器] ──→ [Gain] ──╯
```

* **事件驱动播放**：Tick 由相位累加器触发，Chime（整点报时等）由外部异步事件触发。每个播放器维护独立的读指针，播放完毕后输出静音。
* **Mixer**：支持独立音量调整。公式：$S_{out} = (S_{tick} \cdot G_{m}) + (S_{chime} \cdot G_{c})$。
* **Soft Limiter**：在输出前应用 $\tanh$ 或手动实现 `clamp`，防止多声源叠加导致的数字削波。
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

触觉反馈 (Haptics)：在点击 +1/-1 BPM 时，Android 调用 Vibrator，iOS 调用 UIImpactFeedbackGenerator，给跑者确认感。

常亮模式：运行期间请求 keepScreenOn，防止屏幕自动休眠。

锁定逻辑：开启锁定后，屏蔽 START/PAUSE 以外的所有按钮，防止跑步时汗水或误触误操作。

---

### 6. Android 平台深度优化 (Anti-Kill & Low-Latency)

Android 系统的碎片化和严苛的后台管理是最大挑战。

* **流管理与降级**：
  * 首选 `AAUDIO_PERFORMANCE_MODE_LOW_LATENCY` + `AAUDIO_SHARING_MODE_EXCLUSIVE`。
  * **降级逻辑**：若独占模式失败（常见于蓝牙耳机或国产 ROM 限制），自动回退至 `AAUDIO_SHARING_MODE_SHARED` 模式。

* **XRun 监控**：实时记录采样溢出（Underrun/Overrun），动态调整 Buffer 大小以平衡延迟与稳定性。
* **保活三剑客**：
  1. **Foreground Service**：绑定常驻通知栏。
  2. **WakeLock**：防止 CPU 进入深度休眠。
  3. **Battery Optimization**：引导用户在系统设置中为 App 开启"不限制后台活动"。

---

### 7. iOS 平台深度优化 (System Integration)

* **Audio Session**：配置为 `AVAudioSessionCategoryPlayback`，支持后台持续发声。
* **RemoteIO**：直接控制 `AudioUnit` 回调，绕过高层 API 以获得极致稳定性。
* **锁屏集成**：对接 `MPRemoteCommandCenter`，支持在锁屏界面微调 BPM 或启停。

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

### 10. 项目代码结构建议

```text
/RunBeat
  ├── /core (C++ Shared)
  │     ├── Clock.hpp         // 相位计算核心
  │     ├── Mixer.hpp         // 混音与限幅
  │     └── AudioEngine.cpp   // 状态机与流程控制
  ├── /android
  │     ├── aaudio_engine.cpp  // JNI 与 AAudio 驱动
  │     └── Service.kt        // 前台服务与保活
  └── /ios
        ├── AudioUnit.mm      // Obj-C++ 驱动
        └── RemoteCmd.swift   // 锁屏控制
```

