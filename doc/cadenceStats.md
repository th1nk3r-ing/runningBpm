# 步频实时统计 — 设计方案

## 1. 目标

在节拍器（目标 BPM）之上提供实测步频：

- 实时步频（SPM），1 秒刷新，冷启动约 1.5–2 秒收敛。
- 与目标 BPM 的偏差。
- 本次跑步统计：平均、最高、跟拍率、有效时长、总步数、近 60 秒趋势。
- 熄屏与切后台期间继续采集与累计。
- 算法核心位于 C++ 层，Android 与 iOS 共用同一实现。

分工：**C++ 只算，平台只搬，UI 只画。**

---

## 2. 界面

### 2.1 主页面

```text
┌──────────────────────────────────────────┐
│ ⚙             ● 运行中             🔒    │
├──────────────────────────────────────────┤
│                                          │
│               ╭────────────╮             │
│            ╭──╯            ╰──╮          │
│           ╱   SPM     176    ╲           │
│          │    目标    173.0   │          │
│           ╲            Δ -4  ╱           │
│            ╰──╮        ╭───╯             │
│               ╰────────╯                 │
│                                          │
├──────────────────────────────────────────┤
│              00:32:18              ↻      │
├──────────────────────────────────────────┤
│   1,284 步                       统计 ›  │
├──────────────────────────────────────────┤
│   [ -5 ]   [ -1 ]   [ +1 ]   [ +5 ]      │
│   ┌────────────────────────────────────┐ │
│   │              PAUSE                 │ │
│   └────────────────────────────────────┘ │
├──────────────────────────────────────────┤
│   音量 ▬▬▬▬▬▬▬▬▬▬               x1       │
└──────────────────────────────────────────┘
```

页面自上而下：状态条、环形仪表、计时行、步数 + 统计入口、BPM 微调、启停、音量与倍率。语言、主题色、音色、重音、写日志位于侧滑抽屉。

**环形仪表**

- 圆环范围映射 **150–210 SPM**，目标位置以三角刻度标出；跟踪中按实测值填充。目标 BPM 越界时刻度夹在环的两端。
- 环心为两行「标签 + 数值」：`SPM` / `目标` 为 12sp 小字、右对齐在 52dp 标签列；数值在 100dp 定宽列内水平居中。
- 定宽列的目的：数值中心恒定落在环心（`52 + 10 + 100 + 62 = 224dp`，右侧 62dp 留白与左侧标签列等宽），切状态、换数值宽度都不漂移。第一行高度固定 76dp，`–` 占位时不改变块高。
- 实测步频 62sp（主题色）、目标 38sp（主题色降饱和派生）、状态/偏差 15sp。
- 目标色规则：保留主题色相，饱和度 ×0.62、明度 ×0.96；无色相（白月）退回 `#8E8E93`。环上目标刻度同色，二者成组。
- 数值块整体可点，打开 0.1 精度 BPM 滑块；锁定态禁用。
- 偏差配色：`|Δ| ≤ 3` 主题色，`≤ 6` `#FFC107`，`> 6` `#F44336`；偏差按取整后的整数判定，与显示文本一致。
- 目标变化时数值缩放 `1.0 → 1.10 → 1.0`（180ms），环上刻度 200ms 滑动到新角度；连发节流 200ms。

**状态**

| 状态 | 第 1 行 | 第 2 行 | 第 3 行 |
| --- | --- | --- | --- |
| 停止 | `BPM 173` 62sp 主题色（带小数时 40sp 显示 `173.5`） | 隐藏 | `开始后显示实测步频` |
| 运行中，无步伐信号 | `SPM –` 40sp 次要灰 | `目标 173.0` 派生色 | `等待步伐` |
| 运行中，跟踪中 | `SPM 176` 62sp 主题色 | `目标 173.0` 派生色 | `Δ -4` 偏差色 |
| 暂停 | `SPM 176` 62sp 次要灰 | `目标 173.0` 次要灰 | `已暂停` |

完全停止（长按启停按钮）归零回到第一行状态。锁定时环形仪表与统计照常刷新，仅禁用调节类控件。

### 2.2 统计面板

点击 `统计 ›` 打开底部抽屉 `Dialog`：`Window` 底部对齐、宽度撑满、顶部圆角，无额外依赖。

```text
┌──────────────────────────────────────────┐
│                  ▁▁▁▁                    │
│  本次跑步                            ✕   │
│                                          │
│  ┌────────┬────────┬────────┬─────────┐  │
│  │ 平均178 │ 最高181 │ 跟拍 92%│ 32:18  │  │
│  └────────┴────────┴────────┴─────────┘  │
│                                          │
│  步频趋势（近 60s）          目标 180 ─  │
│  190┤        ╭╮                          │
│  180┼────────╯╰──────╮    ╭╮  ─ ─ ─ ─   │
│  170┤                ╰────╯╰──╯          │
│     └──────────────────────────────      │
│                                          │
│  步数 1,284            当前偏差 -4 SPM   │
└──────────────────────────────────────────┘
```

- 打开期间 1 秒刷新，关闭不影响后台累计。
- 曲线取 native 的 60 点环形缓冲，重开面板保留历史。
- 曲线纵轴以 150–210 SPM 为基准：`[min(150, 有效值, 目标) − 5, max(210, 有效值, 目标) + 5]`，无数据时显示空态。
- 停止状态下打开面板显示上一次运行结果，直到下次 `START` 归零。

### 2.3 数值格式

| 项 | 格式 |
| --- | --- |
| 目标 BPM | `%.1f` |
| 实时/平均/最高步频 | 整数 |
| 偏差 | 带符号整数，`+3` / `-4` |
| 跟拍率 | 整数百分比 |
| 步数 | 千分位 |
| 时长 | `HH:MM:SS` |

---

## 3. C++ 算法层

### 3.0 跑步运动模型

运动学关系：`v = c·L/60`，步周期 `T = 60/c = t_c + t_f`（触地 + 腾空）。步频由步周期决定，与步长解耦，因此变速时步频的变化幅度远小于速度。

生理区间：

| 状态 | 步频 SPM | 步长 m | 速度 m/s |
| --- | --- | --- | --- |
| 走 | 90–130 | 0.6–0.8 | 0.9–1.7 |
| 慢跑 | 140–165 | 0.9–1.2 | 2.1–3.3 |
| 常规跑 | 160–185 | 1.1–1.5 | 2.9–4.6 |
| 快跑 | 185–205 | 1.3–1.8 | 4.0–6.2 |
| 冲刺 | 200–260 | 1.6–2.2 | 5.3–9.5 |

变化率上限：

| 过程 | 典型 | 物理上限 |
| --- | --- | --- |
| 走→跑切换 | 110→165 / 5s ≈ 11 SPM/s | 15 SPM/s |
| 冲刺加速 | 180→240 / 3.5s ≈ 17 SPM/s | 20 SPM/s |
| 匀速自然漂移 | < 1 SPM/s | 2 SPM/s |
| 主动减速 | 180→150 / 4s ≈ 7.5 SPM/s | 10 SPM/s |
| 停止 | 170→0 / 2.5s ≈ 68 SPM/s | 由无步点超时处理 |

单步变化量 `Δc = (dc/dt)·(60/c)`：150 SPM 时 8.0（5.3%），180 时 6.7（3.7%），210 时 5.7（2.7%）。

观测模型：

- 生理步间隔变异 `σ_ε ≈ 2%–4%`，叠加插值误差约 0.5%，合计约 3%（180 SPM 时 ±5.4 SPM）。
- 粗差：漏检一步 `T≈2T̄`（−50%），单步双触发 `T≈0.5T̄`（+100%），摆臂误触发 `T≈0.7T̄`（+43%）。

生理变化 ≤ 5%、噪声 ≤ 6%、粗差 ≥ 43%，三者之间留有约 7 倍间隔，因此入窗门限取 **±12%**。

步频过程用 OU 描述自然波动：`dc = θ(μ−c)dt + σ dW`，`θ ≈ 0.15/s`、`σ ≈ 2.5 SPM/√s`，稳态 `σ_c ≈ 4.6 SPM`；意图变速叠加为有界斜坡 `|u| ≤ 20 SPM/s`（加速）或 `40 SPM/s`（减速）。

### 3.1 文件与接口

| 文件 | 内容 |
| --- | --- |
| `cpp/core/CadenceDetector.hpp` | header-only，检测、统计、曲线缓冲 |
| `cpp/cadence_jni.cpp` | JNI 桥接 |
| `cpp/test/cadence_detector_test.cpp` | 合成信号单测 |
| `cpp/CMakeLists.txt` | `runbeat` 增加 `cadence_jni.cpp`；`RUNBEAT_TESTS` 增加 `cadence_detector_test` |

```cpp
namespace cadence {

struct Snapshot {
    double cadenceSpm;   // 0 = 未跟踪
    double avgSpm;
    double maxSpm;
    double accuracy;     // 0..1
    double activeSec;
    double targetSpm;
    long long steps;
    bool tracking;
    double envelope;     // 当前包络，仅用于真机标定日志
};

class CadenceDetector {
public:
    static constexpr int kHistorySize = 60;

    void Reset() noexcept;
    void SetTarget(double spm) noexcept;
    void SetSampleRate(double hz) noexcept;   // 传入实测采样率，默认 200
    void Push(float x, float y, float z, std::int64_t tsNanos) noexcept;
    Snapshot GetSnapshot() const noexcept;
    void CopyHistory(float* out, int n) const noexcept;   // 0.0 = 该秒无数据
};

} // namespace cadence
```

### 3.2 处理流程

1. 合成幅值 `m = √(x² + y² + z²)`，与手机朝向无关。
2. **两级级联**带通（4 阶）保留 5–25 Hz：每级 RBJ 二阶，`fc = 11.2 Hz`、`Q = 0.56`。手臂摆动（约 1.5 Hz）衰减约 −35 dB，有效通带约 6.5–19 Hz。
3. 整流后取快包络 `E = EMA(|bp|, τ = 0.1s)`。
4. 动态阈值：`|bp|` 上穿 `max(0.55E, 0.6 m/s²)` 判定进入峰值区，回落至 `max(0.25E, 0.2 m/s²)` 重新武装；步点后 100 ms 内不重新武装。
5. 步点时刻取上穿阈值的线性插值位置，消除采样量化误差。
6. 步间隔：不应期 250 ms；有效区间 250–1500 ms；超过 1500 ms 的间隔丢弃，窗口内其余间隔保留。
7. 抗污染：`|60/dt − ĉ| > 12%·ĉ` 判为粗差，计入步数但不入窗；连续 4 次粗差判定为真实换挡，窗口重置并以最后一个间隔重新起步。
8. 步频：`median(最近 5 个间隔)`（不足 5 个用全部）→ `60 / median` → EMA(`α=0.35`)；≥ 4 步出首个值。
9. 显示：变化率限制为加速 20 SPM/s、减速 40 SPM/s；首次获得估计时直接对齐（人无法从 0 缓慢加速到跑步步频）。
10. 跟踪：连续 2.5 s 无步点退出显示；间隔窗口在连续 8 s 无新步点时清空。

### 3.3 统计口径

| 指标 | 定义 |
| --- | --- |
| 步数 | 确认步点总数（含未入窗的离群间隔） |
| 有效时长 | `tracking` 且距上一拍 ≤ 1.5 s 时累计 |
| 平均步频 | `∫ĉ dt / 有效时长`（时间加权，避免按步数相除时的量化跳动） |
| 最高步频 | 进入跟踪 2 s 后平滑值的最大值 |
| 跟拍率 | 跟踪满 2 s 后，`|显示值 − 目标| ≤ 3` 的时长占比（起步收敛期不计入分子分母） |
| 曲线 | 1 Hz 采样平滑步频，60 点环形缓冲；非跟踪秒写 `0.0` |

间隔离群处理：偏离当前平滑步频超过 12% 的间隔计入步数但不入窗；连续 4 次离群视为换挡，窗口重置但保留统计与显示值。

`SetTarget` 只影响后续跟拍率累计，历史数值不重算。

### 3.4 参数

| 参数 | 值 |
| --- | --- |
| 采样率 | 请求 200 Hz；按实测平均间隔校准滤波器 |
| 带通 | 5–25 Hz，两级 RBJ 二阶级联，`fc 11.2 Hz`，`Q 0.56` |
| 包络 | `EMA(|bp|, τ=0.1s)` |
| 上穿 / 重武装 | `max(0.55E, 0.6 m/s²)` / `max(0.25E, 0.2 m/s²)` |
| 重新武装屏蔽 | 步点后 100 ms |
| 不应期 | 250 ms |
| 有效间隔 | 250–1500 ms；超过则丢弃该间隔 |
| 窗口清空 | 连续 8000 ms 无新步点 |
| 退出跟踪 | 2500 ms |
| 间隔窗口 | 中位数，最近 5 个 |
| 离群门限 | 偏离平滑步频 12% |
| 连续离群重置 | 4 次 |
| 平滑系数 | 0.35 |
| 显示斜率 | 加速 20 / 减速 40 SPM/s |
| 显示范围 | 90–260 SPM |
| 首值门槛 | 4 步 |
| 曲线 | 1 Hz × 60 点 |

阈值与包络下限为首版取值，用真机 60 秒跑步日志标定后固化。采样率在启动后 1 秒内由实际平均间隔估计，偏差超过 20% 时按实测值重算带通系数，日志记录请求值与实测值。

### 3.5 线程与性能

- `Push` 由传感器线程调用，检测状态单线程独占。
- `GetSnapshot` / `CopyHistory` 由 UI 线程调用，内部一把轻量 `std::mutex` 拷贝 POD。
- 不进入音频回调线程，`AudioEngine` 的实时安全约束不受影响。
- 每样本固定开销：两次 biquad、两次 EMA、若干比较；内存 O(1)。200 Hz 下每秒约 400 次乘加，可忽略。

---

## 4. Android 接入

### 4.1 采样

- `CadenceSensor`：`SensorManager` + 专用 `HandlerThread`，以 `samplingPeriodUs = 5000`（请求 200 Hz）注册 `TYPE_ACCELEROMETER`，回调中用 `SensorEvent.timestamp` 作为时间戳。
- 启动后 1 秒内统计实际平均间隔，估算真实采样率；偏差超过 20% 时调用 `setSampleRate` 重算带通，并在日志中记录请求值与实测值。
- 不申请 `ACTIVITY_RECOGNITION`，不使用 `TYPE_STEP_DETECTOR`；原始加速度计无需运行时权限。
- 传感器不可用时 `isAvailable() == false`，主页面环形区显示不支持提示，其余功能正常。

### 4.2 生命周期

采样由 `MetronomeService` 持有，随 `currentRunning && !currentPaused` 开关：

| 触发点 | 动作 |
| --- | --- |
| `START` / `RESUME` | `CadenceSensor.start()`；`START` 时 `CadenceNative.reset()` |
| `PAUSE` | `CadenceSensor.stop()`，保留最后一次读数用于变灰显示 |
| 长按停止（`stopPlayback`） | `CadenceSensor.stop()` + `CadenceNative.reset()` |
| `RESTART` | `CadenceNative.reset()`，保持采集中 |
| Service `onDestroy` | `CadenceSensor.stop()` |

Activity 只读数据，销毁与重建不影响采样与统计。

### 4.3 桥接

`CadenceNative` 沿用 `AudioEngine` 的降级模式：静态块加载 `runbeat`，私有 `*Raw` 方法，库未加载时公开方法为空实现。

```java
public final class CadenceNative {
    public static native void reset();
    public static native void setTarget(double spm);
    public static native void setSampleRate(double hz);
    public static native void push(float x, float y, float z, long tsNanos);
    /** [cadence, avg, max, accuracy, activeSec, steps, target, tracking(0/1), envelope] */
    public static native double[] snapshot();
    /** 返回有效点数，out 中 0.0 表示该秒无数据 */
    public static native int history(float[] out);
}
```

### 4.4 界面接线

- `CadenceRingView` 自绘圆环弧与目标刻度，不绘制文字；中心数字、标签、副行由叠放的 `TextView` 承载。
- `CadenceChartView` 自绘 60 点曲线、目标虚线、渐变填充。
- `CadenceStatsDialog` 承载面板，持有 1 s 刷新回调，关闭时移除。
- `MainActivity` 在现有 1 s `timerRunnable` 内读取快照并刷新环形、步数；暂停/停止分支补一次刷新，避免停留在旧值。
- 停止状态下环心显示目标 BPM，沿用 `tvBpm` 的点击弹窗与 `animateBpmChange`。
- 锁定逻辑保持现有范围，环形、步数、统计入口不参与禁用。

### 4.5 日志

统一走 `Logx` 门面，tag 为 `RunBeat-Cadence`，禁止逐样本输出。

| 级别 | 事件 | 内容 |
| --- | --- | --- |
| `error` | 传感器缺失 / `registerListener` 失败 | 一次，界面同时落到「等待步伐」 |
| `warn` | 实测采样率偏离 200 Hz 超过 20% | `rate rebalanced requested=… actual=…`，native 重算带通 |
| `info` | 传感器起停、首次采样率校准 | `sensor started/stopped`、`rate calibrated actual=…` |
| `info` | 跟踪边界 | `tracking acquired spm=… steps=…` / `tracking lost steps=… sec=…` |
| `info` | 会话小结 | START / RESTART / 完全停止时 `session avg/max/acc/steps/sec`，步数为 0 不写 |
| `debug` | 周期汇总 | 跟踪中 1 s、空闲 5 s；`cadence/avg/max/acc/steps/env/tracking/requestedHz/actualHz` |

汇总行两档密度的原因：跟踪中要保留标定与复盘所需的分辨率，空闲时全是 `cadence=0 tracking=0`，按 1 Hz 打点会让文件轮转的可回溯时长从约 3 小时掉到 2.2 小时。真机标定时只看 `tracking=1` 的段落即可。

### 4.6 文案

`values/strings.xml` 与 `values-en/strings.xml` 同步新增：

`label_spm`、`label_target`、`label_delta`、`label_avg`、`label_max`、`label_accuracy`、`label_steps`、`label_elapsed`、`label_waiting`、`label_no_sensor`、`label_stats`、`label_stats_title`、`label_trend_60s`、`msg_start_for_cadence`、`msg_paused`。

颜色新增 `cadence_warn` `#FFC107`、`cadence_bad` `#F44336`。

---

## 5. iOS 复用

- `AudioEngineBridge.h/.mm` 增加 `reset / setTarget / setSampleRate / push / snapshot / history`，内部 `#include "CadenceDetector.hpp"` 持有同一实例。
- Swift 侧 `CadenceMonitor` 用 `CMMotionManager` 以 200 Hz 推送加速度，1 s 读取快照。
- 原始加速度计无需 `NSMotionUsageDescription`。
- 界面复刻同一结构：`CadenceRingView`、`CadenceChartView`、统计面板；`Localizable.strings` 双份补齐。
- C++ 层不改动。

---

## 6. 测试与验收

### 6.1 单测

合成三轴加速度：`9.81 + 摆臂 1.5Hz + 落脚冲击串 + 噪声`，200 Hz 采样并叠加 ≤ ±2.5 ms 时间戳抖动：

| 用例 | 断言 |
| --- | --- |
| 120 / 150 / 180 / 210 SPM 稳态 | 收敛后误差 ≤ 2 |
| 白噪声、静止 | `tracking == false`，`cadenceSpm == 0` |
| 2 s 断流后恢复 | 时长与步数冻结后继续累计，不归零 |
| `Reset()` | 全部字段归零 |
| 采样间隔在 2.5–20 ms 间抖动 | 结果与稳定 5 ms 一致（±2） |
| 目标切换 | 跟拍率按新目标继续累计 |
| 离群间隔 | 单次 2 倍间隔不改变中位数结果，计入步数 |
| 100 Hz 输入 | 带通按实测速率重算后仍保持 ±3 |
| 步频斜坡 150→190 / 8s | 滞后 < 1.5s，终值 ±3 |
| 冲刺 180→240 / 3s | 单调上升，峰值 ≤ 245，不超调 |
| 减速 190→160 / 5s | 滞后 < 1.5s |
| 停止 180→静止 | 2.5s 内失效，不回弹 |
| 走路 110 SPM | 正常显示，不被下限裁剪 |
| 粗差注入（2% 漏步 + 1% 双触发） | 显示 180±5，全程 ≤ 195 |
| 起步（站立→170） | 2s 内出数，不爬坡 |


### 6.2 真机验收

- 手持 180 SPM 摆臂跑 1 分钟：读数 176–184，跟拍率 > 85%。
- 口袋、臂包两种放置各跑 1 分钟：无系统性减半。
- 静止 5 秒：环心回到 `—`，统计停止增长。
- 暂停：最后一次读数变灰，统计冻结；恢复后立即恢复主题色并继续累计。
- 长按停止：全部归零，回到目标 BPM 显示。
- 熄屏 2 分钟：回到前台时长与步数继续增长。
- 无传感器设备：显示不支持提示，节拍器功能正常。

---

## 7. 风险

| 风险 | 应对 |
| --- | --- |
| 阈值依赖手机放置方式 | 包络自适应 + 绝对门限；真机日志标定后固化参数 |
| 部分厂商 ROM 熄屏限制传感器 | 依赖既有前台服务与 `PARTIAL_WAKE_LOCK`；不承诺全部机型 |
| 检测延迟 1.5–2 s | 文案标注实时估算；稳态波动 ≤ ±2 |
| 跑步中调整目标 BPM | 跟拍率按新目标继续累计，不重算历史 |
| 无加速度计 | 降级提示，不影响节拍器 |
| 200 Hz 采样功耗 | 约 0.5–1 mA；仅在运行且未暂停时开启，与音频播放相比可忽略 |

---

## 8. 实施顺序

| 阶段 | 内容 |
| --- | --- |
| 1 | `CadenceDetector.hpp` + 单测 + CMake，`ctest` 通过 |
| 2 | `cadence_jni.cpp`、`CadenceNative`、`CadenceSensor`、Service 接线 |
| 3 | 环形仪表、曲线、统计面板、主页面重排、中英文案 |
| 4 | 真机标定阈值，回填参数表与 `doc/projectTarget.md` |
