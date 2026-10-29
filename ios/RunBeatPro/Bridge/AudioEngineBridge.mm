#import "AudioEngineBridge.h"

#include "AudioEngine.hpp"
#include "Constants.hpp"
#include "Log.hpp"
#include "WavLoader.hpp"

#import <AudioUnit/AudioUnit.h>
#import <AudioToolbox/AudioToolbox.h>

NSString * const RunBeatAudioInterruptionRequiresPauseNotification =
        @"AudioInterruptionRequiresPauseNotification";
NSString * const RunBeatAudioStateChangedNotification =
        @"AudioStateChangedNotification";

static constexpr const char* kBridgeLogTag = "AudioEngineBridge";
static constexpr const char* kAudioUnitLogTag = "AudioUnit";

// 全局 C++ 引擎单例（进程级生命期）
static AudioEngine gEngine;

static constexpr UInt32 kOutputBus = 0;

// 已载样本与负载时采样率追踪。ARC 不允许非平凡 C++ 对象作 ivar，故与 gEngine
// 一样放文件作用域；硬件采样率变化时据此重采样/重新生成（节拍由 Clock 保持）。
static std::vector<float> gTickHiSamples;
static std::vector<float> gTickLoSamples;
static std::vector<float> gChimeSamples;
static bool gHaveChimeSamples = false;
static int gSamplesLoadedSampleRate = 0;
static bool gUseAssetSamples = false;  // false = 仅生成式 fallback tick

@interface AudioUnitDriver : NSObject
- (instancetype)init;
- (BOOL)_createAudioUnit;
- (BOOL)start;
- (void)stop;
@property (nonatomic, readonly) int sampleRate;
@property (nonatomic, readonly, getter=isReady) BOOL ready;
@end

// 播放意图 — 显式 want-to-run 状态，取代由 _isRunning && !_isPaused 反推。
// 这是 start 失败后能够进入退避重试的关键：intent=Running 时重试会重启流。
typedef NS_ENUM(NSUInteger, PlaybackIntent) {
    PlaybackIntentIdle,     // stopped：不再做任何恢复/重试
    PlaybackIntentRunning,  // want-to-run：流重建后必须启动 driver（失败→退避重试）
    PlaybackIntentPaused,   // 用户暂停：流重建但不启动 driver
};

@implementation AudioEngineBridge {
    AudioUnitDriver *_driver;
    int _sampleRate;

    // 播放意图 + 可见状态镜像（Swift 侧 isRunning/isPaused）
    PlaybackIntent _playbackIntent;
    BOOL _isRunning;
    BOOL _isPaused;

    BOOL _needsStreamRebuild;
    BOOL _rebuildRetryScheduled;
    NSUInteger _rebuildRetryAttempts;
    NSUInteger _rebuildRetryGeneration;

    // initializeEngine 幂等闸门：只在 +shared 路径执行一次（publish-once），
    // 二次调用不得用硬编码默认值覆盖已持久化的音量/accent。
    BOOL _initializeEngineCalled;

    // 所有桥接可变状态的唯一串行队列
    dispatch_queue_t _stateQueue;
}

+ (instancetype)shared {
    static AudioEngineBridge *instance = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        instance = [[self alloc] init];
    });
    return instance;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _sampleRate = beat::kDefaultSampleRate;
        _playbackIntent = PlaybackIntentIdle;
        _stateQueue = dispatch_queue_create("com.runbeat.bridge.state", DISPATCH_QUEUE_SERIAL);
        [self initializeEngine];
    }
    return self;
}

/// 幂等初始化：只执行一次，重复调用直接返回（避免覆盖持久化设置）。
- (void)initializeEngine {
    [self _syncState:^() {
        if (self->_initializeEngineCalled) return;
        self->_initializeEngineCalled = YES;

        gEngine.SetSampleRate(self->_sampleRate);
        gEngine.SetTickVolume(beat::kDefaultTickGain);
        gEngine.SetChimeVolume(beat::kDefaultChimeGain);
        gEngine.SetOutputGain(beat::kDefaultOutputGain);
        gEngine.SetAccentMode(0);

        // fallback 木鱼采样（WAV 加载失败时使用；WAV 成功加载后覆盖）
        [self _regenerateFallbackTicksAtSampleRate:self->_sampleRate];

        // 创建 AudioUnit 驱动（只 Open，等待 Start 才启动音频）
        self->_driver = [[AudioUnitDriver alloc] init];

        // 同步驱动实际采样率（与路由变化路径对称），避免首次路由变化前
        // 引擎时钟停在假设的 48000 上；同时按实际率重制样本。
        if (self->_driver.ready && self->_driver.sampleRate > 0) {
            [self setSampleRateInternal:self->_driver.sampleRate];
        }

        ::beat::log::Get(kBridgeLogTag)->info("initialized (sampleRate={})", self->_sampleRate);
    }];
}

// 进程级生命期是刻意的：gEngine 与 driver 由单例持有，App 退出时释放。
/// 如需手动释放（例如 AppExtension 场景）可调用本方法；App 内不会调用。
- (void)destroyEngine {
    [self _syncState:^() {
        [self->_driver stop];
        self->_driver = nil;
        gEngine.Stop();
        self->_playbackIntent = PlaybackIntentIdle;
        self->_isRunning = NO;
        self->_isPaused = NO;
        [self _cancelStreamRebuildRetryLocked];
        ::beat::log::Get(kBridgeLogTag)->info("destroyed");
    }];
}

- (BOOL)startWithBpm:(double)bpm {
    BOOL ok = [self _syncStateReturning:^BOOL() {
        self->_playbackIntent = PlaybackIntentRunning;
        gEngine.Start(bpm);

        // driver 建立/启动失败：意图仍为 Running —— 退避重试会重建流并重启。
        // 此处不 gEngine.Stop()：引擎保持 Running，成功重试即可直接出声；
        // stop/pause 会取消重试并转入 Idle/Paused。成功路径无需通知：Swift
        // 依据返回值自行镜像 isRunning/isPaused。
        if (![self _attemptStreamRestartLockedWithNotify:NO]) {
            self->_isRunning = NO;
            self->_isPaused = NO;
            return NO;
        }
        ::beat::log::Get(kBridgeLogTag)->info("start({:.1f})", bpm);
        return YES;
    }];
    return ok;
}

- (void)stop {
    [self _syncState:^() {
        self->_playbackIntent = PlaybackIntentIdle;
        gEngine.Stop();
        [self->_driver stop];
        self->_isRunning = NO;
        self->_isPaused = NO;
        [self _cancelStreamRebuildRetryLocked];
        ::beat::log::Get(kBridgeLogTag)->info("stop");
    }];
}

- (void)pause {
    [self _syncState:^() {
        self->_playbackIntent = PlaybackIntentPaused;
        gEngine.Pause();
        [self->_driver stop];
        self->_isPaused = YES;
        [self _cancelStreamRebuildRetryLocked];
        ::beat::log::Get(kBridgeLogTag)->info("pause");
    }];
}

- (BOOL)resume {
    BOOL ok = [self _syncStateReturning:^BOOL() {
        self->_playbackIntent = PlaybackIntentRunning;
        gEngine.Resume();
        // 重试链路会先 gEngine.Resume() 再启 driver；成功即回到 Running/未暂停。
        return [self _attemptStreamRestartLockedWithNotify:NO];
    }];
    return ok;
}

// 中断兜底路径：由 AppDelegate 的 interruptionBegan 分支调用 —— 通话中断直接走此路；
// 非通话中断则先尝试 setActive(true) 重新并入混音，失败时才走此路（我们的会话是
// .playback + .mixWithOthers，通常可继续留在混音中，不停流）。
// 此处停流后等待 handleAudioInterruptionEndedShouldResume 恢复。
- (void)handleAudioInterruptionBegan {
    [self _syncState:^() {
        if (self->_isRunning && !self->_isPaused) {
            [self->_driver stop];
            [self _cancelStreamRebuildRetryLocked];
        }
    }];
}

- (void)handleAudioInterruptionEndedShouldResume:(BOOL)shouldResume {
    [self _syncState:^() {
        if (!self->_isRunning || self->_isPaused) return;
        if (!shouldResume) {
            self->_playbackIntent = PlaybackIntentPaused;
            gEngine.Pause();
            [self->_driver stop];
            self->_isPaused = YES;
            [self _cancelStreamRebuildRetryLocked];
            // 此转态源于系统事件，Swift 无法从调用返回值得知 → 显式通知。
            [[NSNotificationCenter defaultCenter]
                    postNotificationName:RunBeatAudioInterruptionRequiresPauseNotification
                                  object:self];
            return;
        }
        if (![self->_driver start]) {
            self->_needsStreamRebuild = YES;
            [self _attemptStreamRestartLockedWithNotify:NO];
        }
    }];
}

- (void)handleAudioRouteChange {
    [self _syncState:^() {
        [self _attemptStreamRestartLockedWithNotify:YES];
    }];
}

// 状态机内部实现：方法名 Locked 后缀即"必须在 _stateQueue 上运行"。

/// 确保 driver 可用并按 _playbackIntent 启动。复用健康 driver，仅在缺失/!ready
/// 时重建；start 失败的 driver 一并丢弃让退避重试重建。返回 NO 时登记
/// _needsStreamRebuild 并安排重试。
/// notify：start/resume 直接调用传 NO（Swift 依返回值镜像）；内部路径（重试、
/// 路由变化、中断结尾）传 YES，投递 StateChanged 通知 —— 这些转态 Swift 无法
/// 由返回值得知。仅在 isRunning/isPaused 真变化时通知。
- (BOOL)_attemptStreamRestartLockedWithNotify:(BOOL)notify {
    const BOOL oldRunning = self->_isRunning;
    const BOOL oldPaused = self->_isPaused;

    if (self->_driver == nil || !self->_driver.ready) {
        self->_driver = nil;
        self->_driver = [[AudioUnitDriver alloc] init];
    }

    // driver 的 sampleRate 每次取实时值：路由切换可改变硬件实际率，复用的
    // driver 也必须重新协商，并按新率重采样/重生成已载样本。
    if (self->_driver.ready && self->_driver.sampleRate > 0) {
        [self setSampleRateInternal:self->_driver.sampleRate];
    }

    if (!self->_driver.ready) {
        self->_needsStreamRebuild = YES;
        [self _scheduleStreamRebuildRetryLocked];
        return NO;
    }

    if (self->_playbackIntent == PlaybackIntentRunning) {
        // 位于 retry 链路时恢复引擎（Resume：Paused→Running；已 Running 则 no-op）
        gEngine.Resume();
        if (![self->_driver start]) {
            // AudioOutputUnitStart 失败的 driver 不可复用：丢弃让重试重建，
            // 否则会永远复用一个打不开的 driver。
            [self->_driver stop];
            self->_driver = nil;
            self->_needsStreamRebuild = YES;
            [self _scheduleStreamRebuildRetryLocked];
            return NO;
        }
    } else {
        // Paused/Idle：保持 driver 不出声（与“先 stop 再按意图决定是否启动”一致）
        [self->_driver stop];
    }

    self->_needsStreamRebuild = NO;
    self->_rebuildRetryAttempts = 0;
    self->_rebuildRetryScheduled = NO;
    if (self->_playbackIntent == PlaybackIntentRunning) {
        self->_isRunning = YES;
        self->_isPaused = NO;
        if (notify && (!oldRunning || oldPaused)) {
            // 内部转态（如静默恢复重试成功）：Swift 侧的 isRunning/isPaused
            // 需要被拉齐，否则 UI 会停在 stopped/paused 而音频已在播放。
            [[NSNotificationCenter defaultCenter]
                    postNotificationName:RunBeatAudioStateChangedNotification
                                  object:self];
        }
    }
    return YES;
}

- (void)_scheduleStreamRebuildRetryLocked {
    if (!self->_needsStreamRebuild
        || self->_playbackIntent != PlaybackIntentRunning
        || self->_rebuildRetryScheduled
    ) {
        return;
    }
    self->_rebuildRetryScheduled = YES;
    const NSUInteger retryGeneration = ++self->_rebuildRetryGeneration;
    const NSUInteger shift = MIN(self->_rebuildRetryAttempts, (NSUInteger)5);
    const NSTimeInterval delay = MIN((NSTimeInterval)(1U << shift), 30.0);
    if (self->_rebuildRetryAttempts < 6) ++self->_rebuildRetryAttempts;

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                 (int64_t)(delay * NSEC_PER_SEC)),
                   self->_stateQueue, ^{
        if (retryGeneration != self->_rebuildRetryGeneration) return;
        self->_rebuildRetryScheduled = NO;
        if (self->_needsStreamRebuild && self->_playbackIntent == PlaybackIntentRunning) {
            // 内部重试路径：成功后的可见状态变化必须通知 Swift。
            [self _attemptStreamRestartLockedWithNotify:YES];
        }
    });
}

- (void)_cancelStreamRebuildRetryLocked {
    ++self->_rebuildRetryGeneration;
    self->_rebuildRetryScheduled = NO;
    self->_rebuildRetryAttempts = 0;
    self->_needsStreamRebuild = NO;
}

// dispatch_assert_queue_not：若未来某路径在 _stateQueue 上重入 dispatch_sync
// 将必然死锁 —— 在 Debug/Release 均以断言提前暴露（iOS 11+ / min target 17）。
- (void)_syncState:(dispatch_block_t)block {
    dispatch_assert_queue_not(self->_stateQueue);
    dispatch_sync(self->_stateQueue, block);
}

- (BOOL)_syncStateReturning:(BOOL (^)())block {
    dispatch_assert_queue_not(self->_stateQueue);
    __block BOOL out;
    dispatch_sync(self->_stateQueue, ^{
        out = block();
    });
    return out;
}

// setter 只写 gEngine 的无锁原子量，不触碰桥接状态机，无需入队。
- (void)setBpm:(double)bpm {
    gEngine.SetBpm(bpm);
}

- (void)setTickVolume:(double)vol {
    gEngine.SetTickVolume(vol);
}

// iOS 侧 chime API（loadChimeWithFileURL/triggerChime）是 PLANNED 功能
// （整点报时），Swift 层暂无调用，接口保留。
- (void)setChimeVolume:(double)vol {
    gEngine.SetChimeVolume(vol);
}

- (void)setAccentMode:(NSInteger)mode {
    gEngine.SetAccentMode((int)mode);
}

- (void)setOutputGain:(double)gain {
    gEngine.SetOutputGain(gain);
}

- (BOOL)loadTickHiWithFileURL:(NSURL *)hiURL tickLo:(NSURL *)loURL {
    BOOL ok = [self _syncStateReturning:^BOOL() {
        std::vector<float> hiSamples;
        std::vector<float> loSamples;

        BOOL hiOk = [self _loadWavFromURL:hiURL into:&hiSamples];
        BOOL loOk = [self _loadWavFromURL:loURL into:&loSamples];

        if (!hiOk || hiSamples.empty()) {
            ::beat::log::Get(kBridgeLogTag)->warn("failed to load tickHi, using fallback");
            hiSamples = WavGenerator::MakeWoodTick(self->_sampleRate, true);
        }
        if (!loOk || loSamples.empty()) {
            ::beat::log::Get(kBridgeLogTag)->warn("failed to load tickLo, reusing tickHi");
            loSamples = hiSamples;
        }

        gEngine.LoadSoundPack(hiSamples.data(), hiSamples.size(),
                              loSamples.data(), loSamples.size());
        gTickHiSamples = hiSamples;
        gTickLoSamples = loSamples;
        // 只有资产真正加载成功 (hiOk && loOk) 才走“重采样”恢复分支；混入
        // 生成式 fallback 时置 false，令采样率变化按新率重新生成而非重采样。
        gUseAssetSamples = hiOk && loOk;
        gSamplesLoadedSampleRate = self->_sampleRate;
        return hiOk && loOk;
    }];
    return ok;
}

- (BOOL)loadChimeWithFileURL:(NSURL *)chimeURL {
    BOOL ok = [self _syncStateReturning:^BOOL() {
        std::vector<float> samples;
        BOOL loaded = [self _loadWavFromURL:chimeURL into:&samples];
        if (loaded && !samples.empty()) {
            gEngine.LoadChimeSamples(samples.data(), samples.size());
            gChimeSamples = samples;
            gHaveChimeSamples = true;
            gSamplesLoadedSampleRate = self->_sampleRate;
        }
        return loaded;
    }];
    return ok;
}

/// 从 bundle URL 加载 WAV 并重采样至引擎采样率（须在 _stateQueue 上调用）
- (BOOL)_loadWavFromURL:(NSURL *)url into:(std::vector<float> *)outSamples {
    if (!url) return NO;

    NSData *data = [NSData dataWithContentsOfURL:url];
    if (!data || data.length == 0) return NO;

    WavLoader loader;
    BOOL ok = loader.Load(
        static_cast<const uint8_t *>(data.bytes),
        data.length,
        _sampleRate
    );

    if (ok) {
        *outSamples = loader.GetSamples();
        ::beat::log::Get(kBridgeLogTag)->info("loaded WAV: {} ({} samples, {} Hz)",
                                              url.lastPathComponent.UTF8String,
                                              outSamples->size(), _sampleRate);
    }
    return ok;
}

- (void)triggerChime {
    gEngine.TriggerChime();
}

- (BOOL)isRunning {
    return [self _syncStateReturning:^BOOL() { return _isRunning; }];
}
- (BOOL)isPaused  {
    return [self _syncStateReturning:^BOOL() { return _isPaused; }];
}

// 采样率同步（须在 _stateQueue 上调用）：同时按新率重载已载样本。
- (void)setSampleRateInternal:(int)sr {
    if (sr == self->_sampleRate) return;
    self->_sampleRate = sr;
    gEngine.SetSampleRate(sr);
    if (gSamplesLoadedSampleRate > 0 && gSamplesLoadedSampleRate != sr) {
        [self _reloadSamplesAtSampleRate:sr];
    }
}

- (void)_reloadSamplesAtSampleRate:(int)sampleRate {
    if (gUseAssetSamples) {
        gTickHiSamples = WavLoader::Resample(gTickHiSamples, gSamplesLoadedSampleRate, sampleRate);
        gTickLoSamples = WavLoader::Resample(gTickLoSamples, gSamplesLoadedSampleRate, sampleRate);
        gEngine.LoadSoundPack(gTickHiSamples.data(), gTickHiSamples.size(),
                              gTickLoSamples.data(), gTickLoSamples.size());
        if (gHaveChimeSamples) {
            gChimeSamples = WavLoader::Resample(gChimeSamples, gSamplesLoadedSampleRate, sampleRate);
            gEngine.LoadChimeSamples(gChimeSamples.data(), gChimeSamples.size());
        }
        ::beat::log::Get(kBridgeLogTag)->info("resampled WAV samples {} → {} Hz",
                                              gSamplesLoadedSampleRate, sampleRate);
    } else {
        [self _regenerateFallbackTicksAtSampleRate:sampleRate];
        ::beat::log::Get(kBridgeLogTag)->info("fallback ticks regenerated at {} Hz", sampleRate);
    }
    gSamplesLoadedSampleRate = sampleRate;
}

- (void)_regenerateFallbackTicksAtSampleRate:(int)sampleRate {
    gTickHiSamples = WavGenerator::MakeWoodTick(sampleRate, true);
    gTickLoSamples = WavGenerator::MakeWoodTick(sampleRate, false);
    gEngine.LoadSoundPack(gTickHiSamples.data(), gTickHiSamples.size(),
                          gTickLoSamples.data(), gTickLoSamples.size());
    gUseAssetSamples = false;
    gSamplesLoadedSampleRate = sampleRate;
}

@end

// 渲染回调 — C 自由函数，必须实时安全：
// refcon 直接是 gEngine（进程级单例），无 Obj-C 消息发送、无内存分配；
// OnAudioCallback 自身负责静音路径；对 ioData 做空保护。
static OSStatus RenderCallback(
    void *inRefCon,
    AudioUnitRenderActionFlags *ioActionFlags,
    const AudioTimeStamp *inTimeStamp,
    UInt32 inBusNumber,
    UInt32 inNumberFrames,
    AudioBufferList *ioData)
{
    AudioEngine *engine = static_cast<AudioEngine *>(inRefCon);
    if (engine == nullptr || ioData == nullptr || ioData->mNumberBuffers < 1) return noErr;
    float *out = static_cast<float *>(ioData->mBuffers[0].mData);
    if (out == nullptr || inNumberFrames == 0) return noErr;
    engine->OnAudioCallback(out, static_cast<int>(inNumberFrames));
    return noErr;
}

@implementation AudioUnitDriver {
    AudioUnit _audioUnit;
    int _sampleRate;
    BOOL _ready;
}

// driver 不持有 bridge 引用：render refcon 直接指向进程级 gEngine，
// bridge↔driver 无循环引用；bridge 单例保证 gEngine 存活至进程结束。
- (instancetype)init {
    self = [super init];
    if (self) {
        _sampleRate = beat::kDefaultSampleRate;
        _ready = [self _createAudioUnit];
        if (!_ready) _sampleRate = 0;
    }
    return self;
}

// 每次读都查询当前硬件实际率 — RemoteIO 不感知路由事件，路由切换可改变
// 实际采样率；由 bridge（在 _stateQueue 上）在重建/复用后拉取该值再同步引擎。
- (int)sampleRate {
    if (_ready && _audioUnit) {
        AudioStreamBasicDescription asbd;
        UInt32 size = sizeof(asbd);
        memset(&asbd, 0, sizeof(asbd));
        if (AudioUnitGetProperty(_audioUnit,
                                 kAudioUnitProperty_StreamFormat,
                                 kAudioUnitScope_Input,
                                 kOutputBus,
                                 &asbd,
                                 &size) == noErr && asbd.mSampleRate > 0) {
            _sampleRate = (int)asbd.mSampleRate;
        }
    }
    return _sampleRate;
}

- (BOOL)isReady { return _ready; }

- (BOOL)_createAudioUnit {
    AudioComponentDescription desc;
    desc.componentType          = kAudioUnitType_Output;
    desc.componentSubType       = kAudioUnitSubType_RemoteIO;
    desc.componentManufacturer  = kAudioUnitManufacturer_Apple;
    desc.componentFlags         = 0;
    desc.componentFlagsMask     = 0;

    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (!comp) {
        ::beat::log::Get(kAudioUnitLogTag)->error("AudioComponentFindNext failed");
        return NO;
    }

    OSStatus status = AudioComponentInstanceNew(comp, &_audioUnit);
    if (status != noErr) {
        ::beat::log::Get(kAudioUnitLogTag)->error("AudioComponentInstanceNew failed: {}", (int)status);
        return NO;
    }

    // Stream Format: Float32, mono, 48kHz（请求值，实际率由 sampleRate getter 回读）
    AudioStreamBasicDescription asbd;
    memset(&asbd, 0, sizeof(asbd));
    asbd.mSampleRate       = beat::kDefaultSampleRate;
    asbd.mFormatID         = kAudioFormatLinearPCM;
    asbd.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    asbd.mBitsPerChannel   = 32;
    asbd.mChannelsPerFrame = 1;
    asbd.mFramesPerPacket  = 1;
    asbd.mBytesPerFrame    = 4;
    asbd.mBytesPerPacket   = 4;

    status = AudioUnitSetProperty(
        _audioUnit,
        kAudioUnitProperty_StreamFormat,
        kAudioUnitScope_Input,
        kOutputBus,
        &asbd,
        sizeof(asbd)
    );
    if (status != noErr) {
        ::beat::log::Get(kAudioUnitLogTag)->error("Set StreamFormat failed: {}", (int)status);
        return NO;
    }

    AURenderCallbackStruct callbackStruct;
    callbackStruct.inputProc       = RenderCallback;
    callbackStruct.inputProcRefCon = reinterpret_cast<void *>(&gEngine);

    status = AudioUnitSetProperty(
        _audioUnit,
        kAudioUnitProperty_SetRenderCallback,
        kAudioUnitScope_Input,
        kOutputBus,
        &callbackStruct,
        sizeof(callbackStruct)
    );
    if (status != noErr) {
        ::beat::log::Get(kAudioUnitLogTag)->error("SetRenderCallback failed: {}", (int)status);
        return NO;
    }

    status = AudioUnitInitialize(_audioUnit);
    if (status != noErr) {
        ::beat::log::Get(kAudioUnitLogTag)->error("AudioUnitInitialize failed: {}", (int)status);
        return NO;
    }

    return YES;
}

- (BOOL)start {
    if (!_ready || !_audioUnit) return NO;
    OSStatus status = AudioOutputUnitStart(_audioUnit);
    if (status != noErr) {
        ::beat::log::Get(kAudioUnitLogTag)->error("AudioOutputUnitStart failed: {}", (int)status);
        return NO;
    }
    ::beat::log::Get(kAudioUnitLogTag)->info("started");
    return YES;
}

- (void)stop {
    if (!_audioUnit) return;
    AudioOutputUnitStop(_audioUnit);
    ::beat::log::Get(kAudioUnitLogTag)->info("stopped");
}

- (void)dealloc {
    if (_audioUnit) {
        AudioOutputUnitStop(_audioUnit);
        AudioUnitUninitialize(_audioUnit);
        AudioComponentInstanceDispose(_audioUnit);
        _audioUnit = nil;
    }
}

@end

static void WriteBridgeLog(spdlog::level::level_enum level, NSString* category, NSString* message) {
    const char* tag = category.length > 0 ? category.UTF8String : "RunBeatPro";
    const char* text = message.UTF8String;
    ::beat::log::Get(tag)->log(level, "{}", text != nullptr ? text : "");
}

@implementation AudioEngineBridge (Logging)

+ (void)setupLogging {
    NSString* base = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject;
    NSString* dir = base.length > 0 ? [base stringByAppendingPathComponent:@"Logs"] : @"";
    if (dir.length > 0) {
        [[NSFileManager defaultManager] createDirectoryAtPath:dir
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:nil];
    }
    // AppState 可能先于本方法初始化，开关状态直接以此处的持久值为准
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    const bool fileEnabled = [defaults objectForKey:@"log_to_file"] == nil
            ? true : [defaults boolForKey:@"log_to_file"];
    ::beat::log::Init({dir.UTF8String, spdlog::level::trace, std::size_t(2) << 20, 3, fileEnabled});
}

+ (void)setFileLoggingEnabled:(BOOL)enabled {
    ::beat::log::SetFileEnabled(enabled);
}

+ (void)logDebug:(NSString *)category message:(NSString *)message {
    WriteBridgeLog(spdlog::level::debug, category, message);
}

+ (void)logInfo:(NSString *)category message:(NSString *)message {
    WriteBridgeLog(spdlog::level::info, category, message);
}

+ (void)logWarn:(NSString *)category message:(NSString *)message {
    WriteBridgeLog(spdlog::level::warn, category, message);
}

+ (void)logError:(NSString *)category message:(NSString *)message {
    WriteBridgeLog(spdlog::level::err, category, message);
}

@end
