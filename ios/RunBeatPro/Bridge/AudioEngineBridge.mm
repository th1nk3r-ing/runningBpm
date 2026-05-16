#import "AudioEngineBridge.h"

// C++ Core — 直接复用
#include "AudioEngine.hpp"
#include "WavLoader.hpp"

#import <AVFoundation/AVFoundation.h>

// ============================================================================
// 前向声明 — AudioUnitDriver 由 AudioEngineBridge 管理生命周期
// ============================================================================
@interface AudioUnitDriver : NSObject
- (instancetype)initWithBridge:(AudioEngineBridge *)bridge;
- (BOOL)start;
- (void)stop;
- (void)setSampleRate:(int)sr;
@property (nonatomic, readonly) int sampleRate;
@end

// ============================================================================
// C++ 全局引擎实例（单例）
// ============================================================================
static AudioEngine gEngine;

// kOutputBus — RemoteIO 的 output bus 索引
static constexpr UInt32 kOutputBus = 0;

// ============================================================================
// AudioEngineBridge 实现
// ============================================================================
@implementation AudioEngineBridge {
    AudioUnitDriver *_driver;
    BOOL _isRunning;
    BOOL _isPaused;
    int _sampleRate;
    double _currentBpm;
    BOOL _needsStreamRebuild;
}

// --------------------------------------------------------------------------
// 单例
// --------------------------------------------------------------------------
+ (instancetype)shared {
    static AudioEngineBridge *instance = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        instance = [[self alloc] _init];
    });
    return instance;
}

- (instancetype)_init {
    self = [super init];
    if (self) {
        _sampleRate = 48000;
        _currentBpm = 180.0;
        _needsStreamRebuild = NO;
        [self initializeEngine];
    }
    return self;
}

// --------------------------------------------------------------------------
// 初始化和清理
// --------------------------------------------------------------------------
- (void)initializeEngine {
    gEngine.SetSampleRate(_sampleRate);
    gEngine.SetTickVolume(0.7);
    gEngine.SetChimeVolume(0.5);
    gEngine.SetOutputGain(1.0);
    gEngine.SetAccent(true);

    // 生成 fallback 木鱼采样（WAV 加载失败时使用）
    std::vector<float> tickHi = WavGenerator::MakeWoodTick(_sampleRate, true);
    std::vector<float> tickLo = WavGenerator::MakeWoodTick(_sampleRate, false);
    gEngine.LoadTickSamples(tickHi.data(), tickHi.size());
    gEngine.LoadTickLoSamples(tickLo.data(), tickLo.size());

    // 创建并启动 AudioUnit 驱动（只 Open，等待 Start 才启动音频）
    _driver = [[AudioUnitDriver alloc] initWithBridge:self];

    NSLog(@"[AudioEngineBridge] initialized (sampleRate=%d)", _sampleRate);
}

- (void)destroyEngine {
    [_driver stop];
    _driver = nil;
    gEngine.Stop();
    NSLog(@"[AudioEngineBridge] destroyed");
}

// --------------------------------------------------------------------------
// 引擎生命周期
// --------------------------------------------------------------------------
- (void)startWithBpm:(double)bpm {
    _currentBpm = bpm;
    gEngine.Start(bpm);
    [_driver start];
    _isRunning = YES;
    _isPaused = NO;
    NSLog(@"[AudioEngineBridge] start(%.1f)", bpm);
}

- (void)stop {
    gEngine.Stop();
    [_driver stop];
    _isRunning = NO;
    _isPaused = NO;
    NSLog(@"[AudioEngineBridge] stop");
}

- (void)pause {
    gEngine.Pause();
    [_driver stop];
    _isPaused = YES;
    NSLog(@"[AudioEngineBridge] pause");
}

- (void)resume {
    gEngine.Resume();
    [_driver start];
    _isPaused = NO;
    NSLog(@"[AudioEngineBridge] resume");
}

// --------------------------------------------------------------------------
// 运行时参数
// --------------------------------------------------------------------------
- (void)setBpm:(double)bpm {
    gEngine.SetBpm(bpm);
}

- (void)setTickVolume:(double)vol {
    gEngine.SetTickVolume(vol);
}

- (void)setChimeVolume:(double)vol {
    gEngine.SetChimeVolume(vol);
}

- (void)setAccent:(BOOL)on {
    gEngine.SetAccent((bool)on);
}

- (void)setOutputGain:(double)gain {
    gEngine.SetOutputGain(gain);
}

// --------------------------------------------------------------------------
// 样本加载 (WAV → C++引擎)
// --------------------------------------------------------------------------
- (BOOL)loadTickHiWithFileURL:(NSURL *)hiURL tickLo:(NSURL *)loURL {
    std::vector<float> hiSamples;
    std::vector<float> loSamples;

    BOOL hiOk = [self _loadWavFromURL:hiURL into:&hiSamples];
    BOOL loOk = [self _loadWavFromURL:loURL into:&loSamples];

    if (!hiOk || hiSamples.empty()) {
        NSLog(@"[AudioEngineBridge] WARNING: failed to load tickHi, using fallback");
        hiSamples = WavGenerator::MakeWoodTick(_sampleRate, true);
    }
    if (!loOk || loSamples.empty()) {
        NSLog(@"[AudioEngineBridge] WARNING: failed to load tickLo, reusing tickHi");
        loSamples = hiSamples;
    }

    gEngine.LoadSoundPack(hiSamples.data(), hiSamples.size(),
                           loSamples.data(), loSamples.size());
    return hiOk && loOk;
}

- (BOOL)loadChimeWithFileURL:(NSURL *)chimeURL {
    std::vector<float> samples;
    BOOL ok = [self _loadWavFromURL:chimeURL into:&samples];
    if (ok && !samples.empty()) {
        gEngine.LoadChimeSamples(samples.data(), samples.size());
    }
    return ok;
}

/// 从 bundle URL 加载 WAV 并重采样至引擎采样率
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
        NSLog(@"[AudioEngineBridge] loaded WAV: %@ (%zu samples, %d Hz)",
              url.lastPathComponent, outSamples->size(), _sampleRate);
    }
    return ok;
}

// --------------------------------------------------------------------------
// 事件触发
// --------------------------------------------------------------------------
- (void)triggerChime {
    gEngine.TriggerChime();
}

// --------------------------------------------------------------------------
// 引擎状态查询
// --------------------------------------------------------------------------
- (BOOL)isRunning { return _isRunning; }
- (BOOL)isPaused  { return _isPaused; }

// --------------------------------------------------------------------------
// 音频回调入口（AudioUnit 渲染线程调用，必须实时安全）
// --------------------------------------------------------------------------
- (void)onAudioCallback:(float *)outBuffer numFrames:(int)numFrames {
    gEngine.OnAudioCallback(outBuffer, numFrames);
}

// --------------------------------------------------------------------------
// 采样率同步
// --------------------------------------------------------------------------
- (void)setSampleRate:(int)sr {
    if (sr == _sampleRate) return;
    _sampleRate = sr;
    gEngine.SetSampleRate(sr);
}

// --------------------------------------------------------------------------
// 流重建
// --------------------------------------------------------------------------
- (BOOL)needsStreamRebuild { return _needsStreamRebuild; }

- (void)rebuildStream {
    _needsStreamRebuild = NO;
    [self destroyEngine];
    [self initializeEngine];
    if (_isRunning && !_isPaused) {
        [self startWithBpm:_currentBpm];
    }
}

// 供 AudioUnitDriver 调用的流异常标记
- (void)_markStreamError {
    _needsStreamRebuild = YES;
}

@end

// ============================================================================
// AudioUnitDriver (RemoteIO)
// ============================================================================

#import <AudioUnit/AudioUnit.h>
#import <AudioToolbox/AudioToolbox.h>

// 渲染回调 — C 函数，必须实时安全
static OSStatus RenderCallback(
    void *inRefCon,
    AudioUnitRenderActionFlags *ioActionFlags,
    const AudioTimeStamp *inTimeStamp,
    UInt32 inBusNumber,
    UInt32 inNumberFrames,
    AudioBufferList *ioData)
{
    @autoreleasepool {
        AudioEngineBridge *bridge = (__bridge AudioEngineBridge *)inRefCon;
        float *out = (float *)ioData->mBuffers[0].mData;
        memset(out, 0, inNumberFrames * sizeof(float));
        [bridge onAudioCallback:out numFrames:(int)inNumberFrames];
    }
    return noErr;
}

@implementation AudioUnitDriver {
    AudioUnit _audioUnit;
    AudioEngineBridge *_bridge;
    int _sampleRate;
}

- (instancetype)initWithBridge:(AudioEngineBridge *)bridge {
    self = [super init];
    if (self) {
        _bridge = bridge;
        _sampleRate = 48000;
        [self _createAudioUnit];
    }
    return self;
}

- (int)sampleRate { return _sampleRate; }

// --------------------------------------------------------------------------
// 创建 RemoteIO AudioUnit
// --------------------------------------------------------------------------
- (BOOL)_createAudioUnit {
    // 1. 找到 RemoteIO 组件
    AudioComponentDescription desc;
    desc.componentType          = kAudioUnitType_Output;
    desc.componentSubType       = kAudioUnitSubType_RemoteIO;
    desc.componentManufacturer  = kAudioUnitManufacturer_Apple;
    desc.componentFlags         = 0;
    desc.componentFlagsMask     = 0;

    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (!comp) {
        NSLog(@"[AudioUnit] ERROR: AudioComponentFindNext failed");
        return NO;
    }

    // 2. 创建实例
    OSStatus status = AudioComponentInstanceNew(comp, &_audioUnit);
    if (status != noErr) {
        NSLog(@"[AudioUnit] ERROR: AudioComponentInstanceNew failed: %d", (int)status);
        return NO;
    }

    // 3. 配置 Stream Format: Float32, mono, 48kHz
    AudioStreamBasicDescription asbd;
    memset(&asbd, 0, sizeof(asbd));
    asbd.mSampleRate       = 48000.0;
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
        kOutputBus,  // output bus (bus 0)
        &asbd,
        sizeof(asbd)
    );
    if (status != noErr) {
        NSLog(@"[AudioUnit] ERROR: Set StreamFormat failed: %d", (int)status);
        return NO;
    }

    // 4. 注册渲染回调
    AURenderCallbackStruct callbackStruct;
    callbackStruct.inputProc       = RenderCallback;
    callbackStruct.inputProcRefCon = (__bridge void *)_bridge;

    status = AudioUnitSetProperty(
        _audioUnit,
        kAudioUnitProperty_SetRenderCallback,
        kAudioUnitScope_Input,
        kOutputBus,
        &callbackStruct,
        sizeof(callbackStruct)
    );
    if (status != noErr) {
        NSLog(@"[AudioUnit] ERROR: SetRenderCallback failed: %d", (int)status);
        return NO;
    }

    // 5. 初始化
    status = AudioUnitInitialize(_audioUnit);
    if (status != noErr) {
        NSLog(@"[AudioUnit] ERROR: AudioUnitInitialize failed: %d", (int)status);
        return NO;
    }

    // 6. 查询实际采样率并同步
    UInt32 size = sizeof(asbd);
    AudioUnitGetProperty(_audioUnit,
                         kAudioUnitProperty_StreamFormat,
                         kAudioUnitScope_Input,
                         kOutputBus,
                         &asbd,
                         &size);
    _sampleRate = (int)asbd.mSampleRate;

    NSLog(@"[AudioUnit] RemoteIO created: %d Hz, %.1f ch",
          _sampleRate, asbd.mChannelsPerFrame);

    return YES;
}

// --------------------------------------------------------------------------
// Start / Stop
// --------------------------------------------------------------------------
- (BOOL)start {
    if (!_audioUnit) return NO;
    OSStatus status = AudioOutputUnitStart(_audioUnit);
    if (status != noErr) {
        NSLog(@"[AudioUnit] ERROR: AudioOutputUnitStart failed: %d", (int)status);
        return NO;
    }
    NSLog(@"[AudioUnit] started");
    return YES;
}

- (void)stop {
    if (!_audioUnit) return;
    AudioOutputUnitStop(_audioUnit);
    NSLog(@"[AudioUnit] stopped");
}

// --------------------------------------------------------------------------
// 采样率变更
// --------------------------------------------------------------------------
- (void)setSampleRate:(int)sr {
    _sampleRate = sr;
}

// --------------------------------------------------------------------------
// 清理
// --------------------------------------------------------------------------
- (void)dealloc {
    if (_audioUnit) {
        AudioOutputUnitStop(_audioUnit);
        AudioUnitUninitialize(_audioUnit);
        AudioComponentInstanceDispose(_audioUnit);
        _audioUnit = nil;
    }
}

@end
