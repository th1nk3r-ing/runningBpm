#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// 通知名常量 — .mm 与 Swift 共用（clang importer 映射为 Notification.Name，勿复制字面量）。
///   RequiresPause → 中断结束需暂停；StateChanged → 可见状态发生 Swift 未知的变化
FOUNDATION_EXPORT NSString * const RunBeatAudioInterruptionRequiresPauseNotification;
FOUNDATION_EXPORT NSString * const RunBeatAudioStateChangedNotification;

/// Obj-C 桥接层 — 封装 C++ AudioEngine，供 Swift 调用
@interface AudioEngineBridge : NSObject

+ (instancetype)shared;

/// 由 +shared 调用一次；重复调用是幂等 no-op。
- (void)initializeEngine;
- (void)destroyEngine;

- (BOOL)startWithBpm:(double)bpm;
- (void)stop;
- (void)pause;
- (BOOL)resume;

- (void)handleAudioInterruptionBegan;
- (void)handleAudioInterruptionEndedShouldResume:(BOOL)shouldResume;
- (void)handleAudioRouteChange;

- (void)setBpm:(double)bpm;
- (void)setTickVolume:(double)vol;     // [0.0, 1.0]
- (void)setChimeVolume:(double)vol;
- (void)setAccent:(BOOL)on;
- (void)setOutputGain:(double)gain;    // x1 / x2 / x3

- (BOOL)loadTickHiWithFileURL:(NSURL *)hiURL tickLo:(NSURL *)loURL;
- (BOOL)loadChimeWithFileURL:(NSURL *)chimeURL;

// PLANNED（整点报时）：Swift 层暂未接线，接口保留。
- (void)triggerChime;

@property (nonatomic, readonly) BOOL isRunning;
@property (nonatomic, readonly) BOOL isPaused;

@end

NS_ASSUME_NONNULL_END
