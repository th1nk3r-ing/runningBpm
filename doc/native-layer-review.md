# Native 层代码评审（重构 / 简化 / 优化）

> 范围：`cpp/core/*.hpp`（共享 C++ 引擎）、`cpp/aaudio_engine.cpp`（JNI + AAudio 驱动）、
> `ios/RunBeatPro/Bridge/AudioEngineBridge.{h,mm}`（Obj-C++ / RemoteIO 桥接）。
> 平台外壳（Android Java、iOS Swift）仅在与 native 的契约相关处提及。
>
> 基线：`cpp` 单元测试 7/7 通过（`ctest`，Release，macOS 主机侧）。
> 文中的文件:行号以当前工作区为准。
>
> 修订记录：v2 — 经独立核验后修正了 3.1（start/resume 拆分）、2.2（因果关系）、
> 1.2/2.1（符号归属）等表述错误，并补充 iOS 对称性问题、Debug-only 重入检测等遗漏项。
> 标注约定：**【已核实】**＝已经代码/测试确认；**【推断】**＝静态推断、尚未复现，需进一步验证。

---

## 0. 总体判断

核心引擎的实时安全设计（相位累加/countdown、atomic 参数交换、换样本闸门、硬限幅）成立，并有单元测试覆盖。
问题集中在三类：

1. **可读性 / 可简化**：`AudioEngine` 的换样本闸门逻辑重复且冗长；一批符号属于 test-only 或死代码。
2. **重复逻辑**：native 两个加载入口、采样率 fallback、常量与默认值在多处复制。
3. **真正的缺陷**：iOS 桥接的 `start` 恢复路径不可达、`initializeEngine` 被二次调用并覆盖用户设置、
   桥接状态缺少并发保护、存在循环引用；native/iOS 在采样率变化后不重载样本。

风险等级：**P0 = 功能性缺陷，P1 = 正确性/健壮性（多为【推断】），P2 = 简化与清理**。

> 补充（【已核实】）：`OnAudioCallback` 里的回调重入检测整段位于 `#ifndef NDEBUG`（`AudioEngine.hpp:149-159`），
> 即 **Release 构建下没有重入保护**。上述“实时安全设计成立”指算法与内存模型层面，重入断言仅 Debug 生效。

---

## 1. 共享核心 C++（`cpp/core`）

### 1.1 【P2】`AudioEngine` 换样本闸门是本文件最重度的复杂度

`AudioEngine.hpp:148-227`（`OnAudioCallback`）与 `:230-244`（`Begin/EndSampleUpdate`）：

- `loading_` 检查出现两次（`:160`、`:172`），中间夹着 `activeCallbacks_.fetch_add` + RAII `CallbackScope`；
  共 **三处** `for (i) out[i]=0` 静音填充（`:161` 首次 loading 检查、`:173` 二次检查、`:177` 状态非 Running）。
- `BeginSampleUpdate` 里 `test_and_set` 自旋 + `std::this_thread::yield()`，`End` 再清标志。
- **多处** `std::memory_order_seq_cst` 刻意保证可见性（`:160/164/167/172/235/236/242` 等），注释解释了 why，但整体心智负担高。

**建议**：抽 `WriteSilence(out, n)` + 一个 `RealtimeSection` RAII 守卫，把入口压成：

```cpp
if (!RealtimeSection::TryEnter(loading_, activeCallbacks_)) { WriteSilence(out, numFrames); return; }
```

行为保持不变；重构后必须继续通过 `engine_test` 的并发用例（`TestConcurrentClockControl`，建议配合 `-DRUNBEAT_TSAN=ON`）。

### 1.2 【P2】test-only / 死代码梳理

区分两类：**test-only**（生产不用，但测试依赖）与**死代码**（全仓库零引用）。

| 符号 | 类别 | 生产链路 | 现状 |
| --- | --- | --- | --- |
| `Mixer.hpp` 全部 | test-only | 否（引擎内联混音，`AudioEngine.hpp` 未 include 它） | 仅 `mixer_test` 使用 |
| `Limiter::Process`（tanh 软限幅） | test-only | 否（生产只用 `ProcessHard`） | 仅 `limiter_test` 使用 |
| `Clock::Process` | test-only | 否（回调用 `Advance`） | 仅 `clock_test` 使用 |
| `Clock::GetFramesToNextTick` | test-only | 否 | 仅 `clock_preserve_phase_test` 使用 |
| `Clock::SetFramesToNextTick` | **死代码** | 否 | 零引用（仅 `Clock.hpp:74` 定义） |
| `SamplePlayer::Render` | test-only | 否 | 仅 `sample_player_test` 使用 |
| `WavGenerator::MakeTick` | **死代码** | 否（实产用 `MakeWoodTick`） | 零引用 |

**建议**：死代码删除；test-only 统一加 `// test-only` 标注（而非删除，保留测试覆盖），不要笼统称“死代码”。
另注：`readme.md` §4 的 pipeline 图同时标注了 `[Mixer]`（`:42`）与 `[Soft Limiter]`（`:48`），
但生产实际是内联混音 + `Limiter::ProcessHard`（`AudioEngine.hpp:226`），两处图文不符，需同步。

### 1.3 【P2】常量 / 默认值分散

`48000` 至少出现在 `AudioEngine.hpp:260,263`、`Clock.hpp:126`、`aaudio_engine.cpp:23,81,399`、
`AudioEngineBridge.mm:69,384,422`。`0.7 / 0.5 / 0.97 / kVoices=4` 同样散落。
**建议**：在 core 暴露一组 `constexpr`（`kDefaultSampleRate`、`kDefaultTickGain`、`kDefaultChimeGain`、`kHardLimit`）供各桥接复用。

### 1.4 【P2，可选】混音循环

`AudioEngine.hpp:218-221` 每帧同时 `ReadOne()` 强/弱两路再二选一。功能正确（两路都推进以保留长尾），
在 4 voice、几十 ms 的 click 场景收益可忽略，是否只推进选中一路属可读性权衡。

---

## 2. JNI / AAudio（`cpp/aaudio_engine.cpp`）

### 2.1 【P1】两个加载入口重复

`nativeLoadWavAssets`（`:72`）与 `nativeLoadSoundPack`（`:386`）各自：

- 计算 `targetRate = (gStreamSampleRate > 0) ? gStreamSampleRate : 48000`（`:81` 与 `:399`，字面重复）；
- 走 `LoadWavFromAsset` 解析路径；
- 写入引擎时入口不同：前者分别调 `LoadTickSamples`/`LoadTickLoSamples`（`:92`/`:99`，另加 `LoadChimeSamples` `:107`），
  后者用一次原子替换的 `LoadSoundPack`（`:427`）。

差异仅在：前者额外加载 chime 且要求三者全成功；后者有“lo 缺失→复用 hi”的 fallback（`:423-425`）。
**建议**：合并为单个 `nativeLoadSamples(mgr, hi, lo, chime)`，用“lo 空→复用 hi；chime null→跳过”的表达，
消除重复以及两套不一致的 fallback 策略。

### 2.2 【P1】采样率变化后不重载样本（Android / iOS 同型）

`RebuildStream`（`:227`）→ `OpenStream` 会刷新 `gStreamSampleRate`（`:197`）并 `gEngine.SetSampleRate`（`:199`）。
但 WAV 是在打开流时按**旧**采样率重采样好的（`:81`/`:399`）。

**影响（修正）**：`Clock::SetSampleRate`（`Clock.hpp:30-37`）会用新采样率重算 `framesPerTick`，
因此 **BPM 的节拍速度保持正确**；受影响的是已按旧率重采样的**音色样本**，表现为 **音高与音色时长漂移**（节拍间隔不受影响）。

iOS 侧同型：`_loadWavFromURL` 按当时 `_sampleRate` 重采样（`.mm:303`），`initializeEngine` 的 fallback 木鱼也按 `_sampleRate` 生成（`.mm:88-89`）；
`handleAudioRouteChange` 会 `setSampleRate:` 更新时钟（`.mm:199`），但不重载样本。

**建议**：记录“样本已加载时的采样率”，采样率变化后重载资源或重建 fallback tick（两端各处理一次）。

### 2.3 【P2】死接口与防御性分支

- `nativeHello`（`:277`）：Java 侧 `AudioEngine.nativeHello()` 声明存在但无调用者。
- `nativeGetXRunCount`（`:440`）：无 Java 调用者。精确表述：**JNI 入口已存在但无调用者**；
  真正的 XRun 计数/上报在 `doc/target.md:140` 仍未完成（`doc/tasks.md:128,138` 打勾的是 JNI 函数本身，不代表“监控已接入”）。
- `nativeSetChimeVolume`（`:365`）/ `nativeTriggerChime`（`:433`）：Java 侧声明存在但**无调用者**；
  chime 样本虽被加载（`nativeLoadWavAssets`→`LoadChimeSamples` `:107`），但从不触发。
- 多通道交错分支（`:127-144`）：`OpenStream` 固定 `channelCount=1`（`:174`），
  即**在当前配置下 `gStreamChannels > 1` 不会被选择**；该分支仍可保留为防御性代码，但应加注释，避免误读为活跃路径。

### 2.4 【P2】日志噪声

`nativeSetBpm / nativeSetTickVolume / nativeSetChimeVolume / nativeSetAccent / nativeSetOutputGain`
每次调用都 `LOGI`（`:354` 等）。BPM 拖动时 logcat 刷屏。建议降级或移除。

---

## 3. iOS 桥接（`AudioEngineBridge.mm`）

### 3.1 【P0】`start` 失败后的恢复路径不可达（真 bug）；`resume` 情况不同

**`startWithBpm`（`:111-130`）——确认“永不恢复”**。失败分支顺序：

```
_isRunning = NO;            // :117
_streamRecoveryRequested = NO;  // :119
_needsStreamRebuild = YES;      // :120
[self handleAudioRouteChange];  // :121
```

`handleAudioRouteChange`（`:194`）内 `shouldResume = _isRunning && !_isPaused`（`:195`）此时必为 false；
若新 driver `ready`，还会把 `_needsStreamRebuild` 清回 NO（`:205`）。而 `_scheduleStreamRebuildRetry`
以 `_streamRecoveryRequested` 为门槛（`:211`），也已在 `:119` 被清。
→ **driver start 失败这条最常见故障，`start` 路径既不重启也不重试。**

**`resume`（`:151-166`）——并非“同型”**。它在 `:152` 置 `_streamRecoveryRequested = YES`，失败分支（`:154-160`）
**并未清回该标志**。因此：

- 若重建后的 driver **打不开**（`!_driver.ready`），`handleAudioRouteChange:203` 会调用 `_scheduleStreamRebuildRetry`，
  门槛满足 → **仍会重试恢复**；
- 仅当重建后的 driver **就绪但 `start` 失败**时，`shouldResume` 为 false，`_needsStreamRebuild` 被清，**不重试**。

**建议**：用显式的 “want-to-run / shouldResume” 语义取代由 `_isRunning && !_isPaused` 反推（还原意图），
统一抽一个 `_restartIfDesired`；确保 `start` 失败后也进入退避重试。

### 3.2 【P0】`initializeEngine` 被调用两次并覆盖持久化设置（真 bug）

第一次：`+shared` → `_init`（`:61-73`）。第二次：`RunBeatProApp.swift:17` 的 `.onAppear`。
`initializeEngine`（`:80-97`）用硬编码 `tick=0.7 / chime=0.5 / gain=1.0 / accent=true`（`:82-85`）写入引擎，
而 `AppState.loadPersistedState` 已把持久化的音量/accent 推给引擎 → 第二次调用将其覆盖，UI 与引擎不一致。
（`onAppear` 也可能触发多次，重复重建 AudioUnit。）

**附带问题（【推断】）**：`initializeEngine` 在 `:94` 新建 driver 后，**未**像 `handleAudioRouteChange:199` 那样
`[self setSampleRate:_driver.sampleRate]`；若首开实际采样率 ≠ 48000，引擎时钟会在首次路由变化前与硬件不一致。

**建议**：`initializeEngine` 幂等化（publish-once），移除 `onAppear` 的重复调用，改为回放当前 `AppState` 值，
并在建 driver 后同步其实际采样率。

### 3.3 【P1／需先确认线程】【推断】桥接状态无并发保护

`_isRunning / _isPaused / _streamRecoveryRequested / _needsStreamRebuild / _rebuildRetryScheduled /
_rebuildRetryAttempts / _rebuildRetryGeneration / _driver / _sampleRate / _currentBpm`（`:41-52`）均为普通 ivar，
无锁、无串行队列。已知主线程路径（AppDelegate 观察者用 `queue: .main`，`dispatch_after(...main_queue)`）安全。
存疑点：`RemoteCommandManager` 的回调由 `MPRemoteCommandCenter` 触发，Apple 未承诺在主线程调用；
若落到非主线程，会与主线程的 `_scheduleStreamRebuildRetry` 竞争 `_rebuildRetryGeneration` 的读改写（`:213,221`）。
**验证前提**：先确认 `MPRemoteCommandCenter` 回调的实际线程（打点或断言）。
**建议**：无论结论如何，都收敛为单条串行 `dispatch_queue`（或 Swift 侧 `@MainActor` + 主线程断言），以消除隐含契约。

### 3.4 【P2】bridge ↔ driver 循环引用

`_driver` 强引用（`:42`）与 `AudioUnitDriver._bridge` 强引用（`:383`）构成环，唯一解环口 `destroyEngine`
（`:99`）全项目从未调用。

**修正**：这**不构成随时间累积的泄漏**——`handleAudioRouteChange` 每次都会 `_driver = nil` 后重建（`:196-198`），
旧 driver 会被释放并在其 `dealloc`（`:515-522`）里 `AudioUnitUninitialize` + `AudioComponentInstanceDispose`。
实际影响仅是：**当前**这一个 driver 直到进程退出才释放（单例本身就是进程级生命期）。

**建议**：`_bridge` 改 `weak`（renderRefCon 保留 `__bridge void*`，由单例保证存活）；
并在终止时显式调用 `destroyEngine`，或明确记录“故意保持进程级生命期”。

### 3.5 【P1】Render 回调的实时安全

`RenderCallback`（`:356-371`）：

- `[bridge onAudioCallback:...]` 是 `objc_msgSend`，实时线程不建议；目标方法仅转发 `gEngine`，
  可改为 C 自由函数直接调用 `gEngine.OnAudioCallback`。
- `@autoreleasepool`（`:364`）无 autoreleased 对象，纯开销。
- `memset`（`:367`）冗余：`OnAudioCallback` 在 Running 路径写满、在其他路径清零（`AudioEngine.hpp:160-178,210-223`）。
- 缺 `ioData` / `mNumberBuffers` / `mBuffers[0].mData` 空指针保护。

### 3.6 【P2】死代码与重复

- `AudioUnitDriver.setSampleRate:`（`:508`）从未调用，且若调用会与桥接自己的 `setSampleRate:` 语义冲突。
- `loadChimeWithFileURL:`（`:283`）/ `triggerChime`（`:317`）：Swift 侧无调用 → iOS 的 chime 也是死的（与 Android 侧对称）。
- `_currentBpm`（`:46,70,112`）只写不读。
- `+shared` 用私有 `_init`（`:57-75`）；直接 `[[self alloc] init]` 会得到未初始化 engine/driver 的对象。
- `@"AudioInterruptionRequiresPauseNotification"` 在 `.mm:9` 与 `AppState.swift:155` 各写一份，建议经 bridging header 暴露常量。
- `Service/PreferencesManager.swift` 整个文件无引用，且其 BPM key（`bpm_bits`）与 `AppState`（`bpm`）不一致，建议删除或统一。

---

## 4. 跨层契约（非 native 本体，未逐条复现，供参考）

平台外壳审计发现与 native 契约相关的风险（均需在 Java/Swift 层单独修）：

- **Android — 静默无声风险**：`nativeDestroy` 之后，service 的 1s 健康轮询 `nativeRebuildStreamIfNeeded`
  只看 `gStreamError`；`gStream==nullptr && !gStreamError` 时永远 no-op，可能静默保持 Running/通知/WakeLock。
  **缓解**：常规销毁路径 `MainActivity.onDestroy`（`:238-241`）会先 `MetronomeService.stopExplicitly(this)`
  （置 `KEY_RUNNING=false` 并停服务），使轮询随 `currentRunning` 停止，因此常规场景被部分封堵；
  风险主要在“Activity 被系统回收但服务状态未同步”的边缘路径。
- **Android — 状态镜像**：native 状态在 Java 侧被多处复制（`AudioEngine.initialized`、Activity 的 `isRunning/isPaused`、
  Service 的 `currentRunning/currentPaused`，以及两套 SharedPreferences：operator 运行态与 `PreferencesManager`）。
- **iOS**：`.began` 中断只停 driver，不更新桥接/`AppState` 状态，`ContentView` 的 1Hz 计时器仍递增。

---

## 5. 建议优先级与批次

| 优先级 | 项目 | 风险 |
| --- | --- | --- |
| P0 | 3.1 `start` 恢复死代码（含 `resume` 子情形）、3.2 `initializeEngine` 二次覆盖 | 中（改状态机，需测试） |
| P1 | 3.3 状态串行化（先验证回调线程）、3.5 RT-safe 回调、2.1 合并加载入口、2.2 采样率变化重载样本（含 iOS） | 中 |
| P2 | 1.1 闸门抽 RAII、1.2 清死代码、1.3 常量集中、2.3/2.4 死接口/日志、3.4 解循环引用、3.6 iOS 清理 | 低（回归网：7 个 ctest 用例） |

**推荐起步**：先做 P2 中低风险项（1.2 死代码清理 + 2.1 合并 native 加载入口 + 1.3 常量集中），
以现有 7 个单测作回归网；P0/P1 的并发与状态机改动单独成批，
并补充针对性测试：iOS `start` 失败的恢复路径、换样本闸门并发用例（`-DRUNBEAT_TSAN=ON`）。
