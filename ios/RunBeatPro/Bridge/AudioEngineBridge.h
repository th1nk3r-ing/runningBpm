#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// Obj-C 桥接层 — 封装 C++ AudioEngine，供 Swift 调用
@interface AudioEngineBridge : NSObject

+ (instancetype)shared;

// ===== 初始化和清理 =====
- (void)initializeEngine;
- (void)destroyEngine;

// ===== 引擎生命周期 =====
- (void)startWithBpm:(double)bpm;
- (void)stop;
- (void)pause;
- (void)resume;

// ===== 运行时参数 =====
- (void)setBpm:(double)bpm;
- (void)setTickVolume:(double)vol;     // [0.0, 1.0]
- (void)setChimeVolume:(double)vol;
- (void)setAccent:(BOOL)on;
- (void)setOutputGain:(double)gain;   // x1 / x2 / x3

// ===== 样本加载 =====
- (BOOL)loadTickHiWithFileURL:(NSURL *)hiURL tickLo:(NSURL *)loURL;
- (BOOL)loadChimeWithFileURL:(NSURL *)chimeURL;

// ===== 事件触发 =====
- (void)triggerChime;

// ===== 引擎状态 =====
@property (nonatomic, readonly) BOOL isRunning;
@property (nonatomic, readonly) BOOL isPaused;

// ===== AudioUnit 回调入口（由 AudioUnitDriver 调用） =====
- (void)onAudioCallback:(float *)outBuffer numFrames:(int)numFrames;

// ===== 采样率同步 =====
- (void)setSampleRate:(int)sr;

// ===== 流重建标记 =====
@property (nonatomic, readonly) BOOL needsStreamRebuild;
- (void)rebuildStream;

@end

NS_ASSUME_NONNULL_END
