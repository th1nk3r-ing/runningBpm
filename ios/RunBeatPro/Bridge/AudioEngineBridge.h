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
- (void)setAccentMode:(NSInteger)mode;  // 0=全部重音 1=全部轻音 2=每 2 拍重音 3=每 4 拍重音
- (void)setOutputGain:(double)gain;    // x1 / x2 / x3

- (BOOL)loadTickHiWithFileURL:(NSURL *)hiURL tickLo:(NSURL *)loURL;
- (BOOL)loadChimeWithFileURL:(NSURL *)chimeURL;

// PLANNED（整点报时）：Swift 层暂未接线，接口保留。
- (void)triggerChime;

@property (nonatomic, readonly) BOOL isRunning;
@property (nonatomic, readonly) BOOL isPaused;

@end

/// 统一日志门面：与 C++ / Android 共用级别、格式与轮转文件。
@interface AudioEngineBridge (Logging)

/// 幂等；App 启动时调用一次即可。
+ (void)setupLogging;

/// 面板开关：关闭后仍走 os_log，仅停止写文件。
+ (void)setFileLoggingEnabled:(BOOL)enabled;

+ (void)logDebug:(NSString *)category message:(NSString *)message;
+ (void)logInfo:(NSString *)category message:(NSString *)message;
+ (void)logWarn:(NSString *)category message:(NSString *)message;
+ (void)logError:(NSString *)category message:(NSString *)message;

@end

NS_ASSUME_NONNULL_END
