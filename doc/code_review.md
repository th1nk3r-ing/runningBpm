# RunBeat Pro 代码 Review

> 审查日期：2026-05-14
> 审查范围：C++ Core（Phase 2）+ Android Shell（Phase 3）全部源码

---

## 一、项目概览

这是一个专业级跑步节拍器应用，采用跨平台 Core+Shell 架构：C++17 音频引擎（Core）+ Android Shell（AAudio）。Phase 1-3 已完成，Phase 4 (iOS) 待开发。

---

## 二、严重问题（需优先修复）

### 1. `status_dot.xml` 语法错误 — XML 属性名错误

`status_dot.xml:3` 使用了 `android-color`（无冒号），应为 `android:color`。Android 资源解析器会忽略该属性，导致 inactive 状态圆点**无填充色**（透明）。

```xml
<solid android-color="#4CAF50" />   <!-- 错误 -->
<solid android:color="#4CAF50" />   <!-- 正确 -->
```

### 2. `OnAudioCallback` 中 `tickPlayer_` 和 `tickLoPlayer_` 同时 ReadOne — accent 模式逻辑缺陷

`AudioEngine.hpp:148-155`：

```cpp
if (clock_.Advance()) {
    if (accent) {
        tickPlayer_.Play();     // 只 Play 强拍
    } else {
        tickLoPlayer_.Play();   // 只 Play 弱拍
    }
}
float tickSample = tickPlayer_.ReadOne() + tickLoPlayer_.ReadOne();
```

当 accent 开启时，每次 tick 只触发 `tickPlayer_`，但 `tickLoPlayer_` 仍通过 `ReadOne()` 被读取。如果 `tickLoPlayer_` 有残留播放（未被 Stop 清除、或上一轮 accent=false 时仍在播放的长尾），其输出会叠加到强拍上，造成干扰。

**建议**：在 tick 触发时，如果 accent=true，应显式停止非活跃 player，或在 ReadOne 时根据当前 accent 决定是否跳过非活跃 player：

```cpp
float tickSample = accent ? tickPlayer_.ReadOne() : tickLoPlayer_.ReadOne();
```

### 3. `nativeInit()` 中无条件 `AAudioStream_requestStart` — 千万不要这么做

`aaudio_engine.cpp:253-254`：

```cpp
if (gStream) {
    AAudioStream_requestStart(gStream);
}
```

`nativeInit()` 在 `onCreate()` 中调用，此时用户还没按 START，但音频流已经打开且启动了。这会导致：

- **后台静默占用音频资源**：AAudio EXCLUSIVE 模式下独占音频设备，其他 App 无法使用音频
- **持续消耗 CPU**：data callback 每秒调用数千次（虽然引擎 state=Idle 输出静音）
- **违反 Android 音频焦点规范**

**建议**：`nativeInit()` 中只 OpenStream 不 requestStart，Start 操作推迟到用户点击 START 后执行。同时在 `nativeStart()` 中启动流，`nativeStop()` 中停止流。

### 4. WakeLock 无超时 — 潜在死锁

`MetronomeService.java:88-91`：

```java
wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "RunBeat:MetronomeWakeLock");
wakeLock.acquire();  // 无超时参数！
```

如果 Service.onDestroy() 因异常未被调用（Android 系统强制杀死进程），WakeLock 不会被释放，CPU 将永远不休眠，严重耗电。

**建议**：`acquire()` 时设置超时：

```java
wakeLock.acquire(4 * 3600 * 1000L); // 4 小时上限
```

### 5. BPM 精度问题 — `double` ↔ `float` 降精度

`PreferencesManager.java:33-34`：

```java
public void setBpm(double bpm) {
    prefs.edit().putFloat(KEY_BPM, (float) bpm).apply();
}
```

SharedPreferences 没有 `putDouble`，所以用 `float` 存储。`float` 精度约 7 位有效数字，对 BPM 值（如 180.0）尚可，但如果未来支持小数 BPM（如 180.3），`float` 的 3.3 变成约 3.29999995，可能导致微小累积漂移。

**建议**：改用 `putLong` 存储 double 的位模式（`Double.doubleToRawLongBits`），读取时 `Double.longBitsToDouble`，零精度损失。

---

## 三、重要问题（建议修复）

### 6. Pause/Resume 未管理 AAudio 流 — 资源浪费

`toggleStartPause()` 中 Resume 时只调 `nativeResume()`，音频流持续运行。Pause 期间 data callback 仍在被调用（只是输出静音），浪费 CPU 周期，EXCLUSIVE 模式下仍独占音频设备。

**建议**：Pause 时 `AAudioStream_requestPause()`，Resume 时 `AAudioStream_requestStart()`。这样 Pause 期间不占用音频资源。

### 7. Service 暂不停止 — 注释掉的 stopService

`MainActivity.java:517-518`：

```java
// 暂不停止 Service（后台保持）
// stopService(new Intent(this, MetronomeService.class));
```

用户 PAUSE 后前台服务仍运行 + WakeLock 持续持有，不合理。至少应该在用户明确 STOP 时停止服务。

### 8. 缺少电池优化引导

设计文档明确要求引导用户关闭电池优化（Phase 3.5），但 `MainActivity` 和 `MetronomeService` 中没有 `ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS` 的引导代码。

### 9. 无音频焦点请求（Audio Focus）

播放节拍声前应请求 `AUDIOFOCUS_GAIN_TRANSIENT`，否则与其他 App 音频冲突。AAudio 层面没有音频焦点管理，需在 Java 层通过 `AudioManager.requestAudioFocus()` 实现。

### 10. `gMonoBuf` 向量在音频回调中可能的内存分配

`aaudio_engine.cpp:108-120`：

```cpp
static std::vector<float> gMonoBuf;
// ...
if (static_cast<int32_t>(gMonoBuf.size()) < numFrames) {
    gMonoBuf.assign(static_cast<size_t>(numFrames), 0.0f);  // 可能 realloc！
}
```

`std::vector::assign` 在 capacity 不够时会 malloc，在音频线程上执行内存分配是**实时安全违规**。虽然实际中 burst 大小通常稳定（不会变大），但首次调用时必然触发分配。

**建议**：在 `nativeInit()` 中预分配足够大的 buffer（如 4096 frames），避免回调中分配。

### 11. `chimePlayer_` 无样本加载跟随音色包

`nativeLoadWavAssets` 加载了 chime 样本，但 `nativeLoadSoundPack` 只替换 tick 强拍/弱拍，**不替换 chime**。如果用户换音色包后触发 chime，chime 声音仍是最初的默认音。这可能是设计意图，但建议明确文档化或提供 chime 音色切换选项。

### 12. `nativeLoadWavAssets` 中 `sampleRate` 硬编码 48000

`aaudio_engine.cpp:78-79`：

```cpp
int targetRate = 48000;
```

但代码注释和 `OpenStream()` 都提到"查询实际采样率"。若设备采样率不是 48000（如某些蓝牙设备 44100），WAV 重采样到 48000 后引擎又被 `SetSampleRate(gStreamSampleRate)` 设置为实际 SR，则样本与引擎不一致，导致音高偏移。

**建议**：应使用 `gStreamSampleRate` 作为 `targetRate`，或者在 `nativeInit()` 中先 `OpenStream()` 拿到实际 SR 后再加载样本。

---

## 四、设计/架构建议

### 13. Clock.hpp 的 `Process()` 方法分配 `std::vector` — 不适合实时路径

`Clock.hpp:97-108` 的 `Process()` 创建并填充 `std::vector<int>`，有内存分配。文档注释也明确说"仅用于测试"。但 `Advance()` 是实时安全的，这是正确的设计。当前代码在音频回调中只用 `Advance()`，没问题。✅

### 14. SamplePlayer 双缓冲 + Voice Pool 设计良好

`Load()` 使用 seq_cst 原子翻转索引确保数据完整性，`Play()` 的 voice 复用策略（找空闲或最接近结束的 voice）合理。代码注释清晰。这是一个非常好的实现。✅

### 15. 倍率按钮 `btnGain` 用 `TextView` 而非 `Button`

`activity_main.xml:262-271` 中 `btnGain` 是 `TextView`，但设置了 `clickable` 和 `focusable`。虽然在 Java 中通过 `setOnClickListener` 工作了，但无障碍访问（accessibility）方面，`Button` 更合适，会自动获得 role 描述。

### 16. BPM 范围 120-220 vs 设计文档 150-190

设计文档要求 BPM 150-190（0.1 精度），但代码 `adjustBpm()` 约束为 120-220 且只有整数步进。文档与实现不一致，应统一或更新文档。

### 17. `restartMetronome()` 不暂停 Service

`restartMetronome()` 调用 `nativeStop()` + `nativeStart()`，但没有先停后启 Service。如果引擎状态机发生异常（Stop 未真正停止旧流），可能产生两个并行流。

### 18. CI 中 NDK 版本可能不一致

`build.yml:32` 安装 `ndk;25.2.9519653`，`app/build.gradle:52` 也指定 `ndkVersion = '25.2.9519653'`。一致，但 CI 是 `ubuntu-latest`，Android Gradle Plugin 8.2 并不默认支持该 NDK 版本，如果未来 AGP 升级需注意兼容性。

---

## 五、代码质量 — 小问题

### 19. 硬编码十六进制颜色在多处 Drawable XML

`btn_bpm_control.xml`, `btn_lock.xml`, `bg_selector_row.xml` 等硬编码 `#FF6B35`（橙色），但 Java 代码 `applyAccentColor()` 在运行时动态替换背景。这导致第一个渲染帧显示的是 XML 硬编码色，瞬间切换到主题色，造成闪烁。

### 20. `status_dot.xml` 颜色与设计不符

设计文档中 inactive 状态应该灰色（而非绿色 #4CAF50），active 状态才是绿色。当前两个 drawable 都是绿色，只是 inactive 那个因属性名错误实际透明——这反而是"歪打正着"让 inactive 变透明/无色了。

### 21. 缩进不一致

`MainActivity.java:495-496`：

```java
AudioEngine.nativeStart(bpm);  // 缩进不对，缺少一级缩进
```

### 22. `MetronomeService` 通知不动态更新

通知文字固定 `"Running"`，不反映实际 BPM 或 PAUSED 状态。跑步时用户看通知栏无法得知当前状态。

### 23. `WavGenerator::MakeWoodTick` 使用固定种子 PRNG

`WavLoader.hpp:241`：`seed = accent ? 1337u : 7331u;` — 确定性噪声，每次生成完全相同的 click。这是**设计选择**而非 bug，保证了不同设备上音色一致。✅

### 24. Release 签名使用 `signingConfigs.debug`

`app/build.gradle:38`：正式发布 APK 用 debug 签名，不适合生产环境。需要添加 release keystore 配置。

---

## 六、测试覆盖评估

| 模块 | 覆盖情况 | 说明 |
|------|----------|------|
| Clock.hpp | ✅ 充分 | 4 个测试文件覆盖核心功能（tick 计数、相位精度、PreservePhase、频繁微调） |
| SamplePlayer.hpp | ⚠️ 基本覆盖 | 基本功能覆盖（播放/停止/gain），但**缺少多 voice 并发测试**（如快速连续触发 Play 是否正确 voice 分配） |
| Mixer.hpp | ✅ 充分 | 5 个测试覆盖所有增益组合 |
| Limiter.hpp | ✅ 充分 | 5 个测试含单调性验证 |
| AudioEngine | ⚠️ 基本覆盖 | 集成测试（tick 间隔分析），但**缺少 Pause/Resume/SetBpm 运行中变更的测试** |
| Android 层 | ❌ 无覆盖 | 完全没有自动化测试 |

---

## 七、总结

| 类别 | 数量 | 关键项 |
|------|------|--------|
| 严重问题 | 5 | XML语法错误、accent ReadOne叠加、nativeInit无条件启动流、WakeLock无超时、BPM精度 |
| 重要问题 | 7 | Pause不管理流、Service不停止、无音频焦点、无电池优化引导、gMonoBuf分配、chime不更新、采样率硬编码 |
| 设计建议 | 6 | BPM范围不一致、btnGain无障碍、restart不管理Service、NDK版本等 |
| 小问题 | 6 | XML硬编码色、通知不动态、缩进、release签名 |
| 测试缺失 | 3 | SamplePlayer多voice、引擎Pause/Resume、Android层自动化 |

**总体评价**：C++ Core 的音频引擎设计质量很高——相位累加器消除累积误差、per-sample 处理杜绝回调边界抖动、atomic 无锁参数交换符合实时安全要求。Voice Pool + Double Buffer 的 SamplePlayer 是专业级设计。主要风险集中在 Android Shell 层：AAudio 流生命周期管理（过早启动/不按需暂停）、缺少音频焦点和电池优化引导、WakeLock 安全。建议优先修复前 5 个严重问题后再进入 iOS Phase 4 开发。