# 最近 3 个 Commit 审查 — 剩余问题处理记录

> 承接第一轮审查（`57c7593` / `c9b89d0` / `0b20961`）。
> 后续代码改动已处理可修复项；本文保留原审查依据，并记录处理结论。
> 更新日期：2026-10-12

## 状态汇总

| 类别 | 数量 | 条目 |
|---|---|---|
| ✅ 已处理 | 8 | R1、R2、R3、R4、R5、R6、O1、O2 |
| ℹ️ 有意保留 | 1 | R7（严格拒绝非标准/截断 WAV） |

---

## R1 — ✅ 无签名环境下的 Release 验证 【High】

- **来源**：`57c7593` 发现 #1
- **位置**：`android/app/build.gradle:38-55`
- **处理**：只有四项签名参数齐全时才配置 Release 签名；仅缺签名参数时允许产出文件名带 `-unsigned` 的未签名 APK，以验证 R8/minify。若提供了 keystore 路径但其他签名参数不全，则在配置阶段明确报错。
- **验证**：清除全部 `RUNBEAT_RELEASE_*` 环境变量后，`:app:assembleRelease` 成功并产出 `RunBeatPro-…-release-unsigned.apk`；CI publish 路径仍使用签名 keystore。

---

## R2 — ✅ 8 字节原子的 RT 保证 【Low】

- **来源**：`0b20961` 发现 #5
- **位置**：`cpp/core/AudioEngine.hpp:239`（`std::atomic<std::uint64_t> startGeneration_`），回调侧读取 `:171 startGeneration_.load(acquire)`；`android/app/build.gradle` 发行 `armeabi-v7a`。
- **核验**：该风险在某些 32 位目标上确实可能存在，但项目实际使用的 NDK 25.2、`armv7a-linux-androideabi26` 对 `atomic<uint64_t>` 与 `atomic<double>` 的 `is_always_lock_free` 均为真；arm64-v8a 和 armeabi-v7a native 构建也通过。
- **处理**：在 `AudioEngine.hpp` 对这两种回调内原子类型加入 `static_assert`，避免未来 ABI/toolchain 变化时静默引入 RT 锁。

---

## R3 — ✅ 编译产物 `cpp/test/clock_test` 从待提交版本移除 【Nit】

- **来源**：`57c7593` 发现 #11
- **位置**：`cpp/test/clock_test`（无扩展名二进制），与 `cpp/test/clock_test.cpp` 并存。
- **处理**：工作区中的 Mach-O 测试二进制已删除（待提交的删除会解除跟踪），并在 `.gitignore` 精确忽略该路径；源码 `cpp/test/clock_test.cpp` 保留。

---

## R4 — ✅ TSan 已接线 【High】

- **来源**：`57c7593` 发现 #10 / `0b20961` 发现 #1
- **位置**：`cpp/test/engine_test.cpp`（`TestConcurrentClockControl`）、`cpp/CMakeLists.txt:34-51`、`.github/workflows/build.yml`（"Test C++ core" 步骤）。
- **已做**：oracle 已增强 —— 压力循环后确定性地跑一次 `SetSampleRate(24000)+Start(60)`，断言 `firstTick == 23999`；测试注释也已更正为"该确定性检查无法证明线程归属，需 TSan"。
- **处理**：新增 `RUNBEAT_TSAN` CMake 选项，并在 GitHub Actions 添加独立 Linux job，以 TSan 构建、运行 `engine_test`。
- **验证**：本机 TSan 配置、编译及 `engine_test` 通过；CI job 将在后续 workflow 运行中提供持续覆盖。

---

## R5 — ✅ 单回调线程假设有 Debug 检查 【Low–Medium】

- **来源**：`0b20961` 发现 #4
- **位置**：`cpp/core/AudioEngine.hpp`（线程模型注释，`:16-20` 一带）、`clock_`/`appliedSampleRate_`/`appliedStartGeneration_`。
- **已做**：注释已明确"单一、串行的音频回调线程（AAudio/AudioUnit）……样本更新会等待所有在途 callback；Clock 和 voice 状态仍要求 callback 不重入"。
- **处理**：保留单一串行回调的宿主契约，并在 Debug 构建用 `atomic_flag` 检测回调重入、触发断言。Release 回调不增加该检查开销。

---

## R6 — ✅ 内存序已统一/收窄 【Nit】

- **来源**：`0b20961` 发现 #6
- **位置**：`cpp/core/AudioEngine.hpp:87`（`SetSampleRate` → `release`）、`:164`（读取 `acquire`），以及 `Pause/Resume/SetBpm` 用 `relaxed` 而 `state_` 用 `acquire`。
- **已做**：加入了合并语义注释（"最新值请求；callback 之间的多次采样率更新会合并为最后一个值"）。
- **处理**：`requestedSampleRate_` 是独立的最新值，存取改为 `relaxed`；`state_` 的生命周期写入统一为 `release`，回调/Resume 读取用 `acquire`。`SetBpm` 操作的是独立原子 payload，不与 `state_` 的内存序配对。

---

## R7 — ℹ️ WavLoader 严格性：有意保留 【Medium（取舍）】

- **来源**：`57c7593` 发现 #4
- **位置**：`cpp/core/WavLoader.hpp:13`
- **现状**：行为未变，仅新增注释 *"要求 RIFF/chunk 声明不越过实际数据范围，奇数长度 chunk 带 RIFF padding；拒绝截断或占位长度文件。"* 即把"严格"确认为有意设计。
- **影响**：部分使用占位长度、声明越界或截断的第三方/流式 WAV 会加载失败；这些不是格式长度声明正确的标准 RIFF 文件。`data size=0` 也不能笼统认为旧解析器曾能正常播放。
- **决定**：保留严格边界校验及说明；内置资源不受影响。若产品以后要兼容特定流式来源，应针对其格式单独增加安全的容错路径和测试。

---

## 附带新观察（本批修复引入，非第一轮问题）

### O1 — ✅ `MetronomeService.onDestroy` 区分显式停止与系统回收
- **位置**：`android/app/src/main/java/com/runbeat/pro/MetronomeService.java:142-150`（`onDestroy` 已移除 `persistPlaybackState()`，改为注释说明）
- **处理**：显式停止统一走 `MetronomeService.stopExplicitly()`，写入专用标记并清除恢复状态；`onDestroy()` 仅在读到该标记时再次持久化停止状态。未标记的系统 teardown 保留恢复意图。

### O2 — ✅ iOS 流重建重试受恢复意图控制
- **位置**：`ios/RunBeatPro/Bridge/AudioEngineBridge.mm`（`_scheduleStreamRebuildRetry`）
- **处理**：保留明确恢复意图期间的指数退避（最长间隔 30s）；显式停止、用户暂停或不自动恢复的中断结束时取消待执行重试并使旧 block 失效。失败的显式 Resume 仍允许后台重建驱动，但不会自动开始播放。

---

## 附：已修复（本批未提交改动覆盖）

| 来源 | 问题 | 状态 |
|---|---|---|
| `57c7593` #2 | `AudioEngine.destroy()` native 流泄漏 | ✅ 无条件 `nativeDestroy()` |
| `57c7593` #3 | iOS 非 `.shouldResume` 假运行 | ✅ 桥接暂停 + 通知 → `AppState` |
| `57c7593` #5 | `loadSoundPack` 恒真 | ✅ 改 `jboolean` 并传递 |
| `57c7593` #6 | iOS `_needsStreamRebuild` 死代码 | ✅ 重试接线 |
| `57c7593` #7 | chime 队列熬过 `Pause` | ✅ `Pause` 清队列 + 非 Running 忽略 + 测试 |
| `57c7593` #8 | `MetronomeService` 通知闪现 | ✅ 重排通知更新并按持久状态初始化 |
| `57c7593` #9 | 冗余焦点请求 / 旋转放弃焦点 | ✅ 去重 + 移到配置变更判断内 |
| `c9b89d0` #1–#6 | 注释准确性/知识损失 | ✅ 全部修正 |
| `0b20961` #2 | chime/sample 同类竞争 | ✅ `BeginSampleUpdate`/`activeCallbacks_` 串行化 |
| `0b20961` #3 | `TestDeferredClockUpdates` 无区分度 | ✅ 注释更正 |
| 本轮 | R1/R3/R4 | ✅ 无签名 Release 可验证、移除跟踪二进制、CI 接入 TSan |
| 本轮 | R2/R5/R6 | ✅ 回调原子静态断言、Debug 重入断言、内存序收窄 |
| 本轮 | O1/O2 | ✅ Service 显式停止标记、iOS 停止/暂停时取消重试 |
