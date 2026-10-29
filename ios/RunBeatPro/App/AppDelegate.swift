import UIKit
import AVFoundation
import CallKit

/// App 生命周期代理 — 负责提前配置 AudioSession、区分通话中断
class AppDelegate: NSObject, UIApplicationDelegate, CXCallObserverDelegate {

    private var audioSessionObservers: [NSObjectProtocol] = []

    /// 通话观测器 — 无需 entitlement。用于区分「来电/去电」这类必须暂停的中断与
    /// 「音乐 App 抢占」这类可继续混音的瞬态中断：mixWithOthers 下 `setActive(true)`
    /// 对通话也可能「假成功」，从而静默丢失节拍。
    private let callObserver = CXCallObserver()

    /// 通话结束边沿检测用（响铃 / 接通 / 保持均视为进行中）。
    private var callInProgress = false

    /// 因中断停用了 driver、等待恢复；`.ended` 恢复时清闸。
    private var suspendedForInterruption = false
    /// 上述停用是否由通话引起 — 决定「通话结束」兜底路径是否负责恢复。
    private var suspendedForCall = false
    /// 通话结束兜底是否已替 `.ended` 完成恢复。若已恢复，迟到的 `.ended` 不得再
    /// 重启 driver —— 保证「单次、幂等」的恢复。
    private var fallbackDidResume = false

    func application(
        _ application: UIApplication,
        didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil
    ) -> Bool {
        AudioEngineBridge.setupLogging()
        configureAudioSession()
        observeAudioSessionEvents()
        callObserver.setDelegate(self, queue: .main)
        // 初始化时同步一次当前通话状态。
        callInProgress = !callObserver.calls.isEmpty
        return true
    }

    deinit {
        callObserver.setDelegate(nil, queue: nil)
        removeAudioSessionObservers()
    }

    // MARK: - AudioSession

    private func configureAudioSession() {
        let session = AVAudioSession.sharedInstance()
        do {
            try session.setCategory(
                .playback,
                mode: .default,
                options: [.mixWithOthers]
            )
            try session.setActive(true)
            AudioEngineBridge.logInfo("AudioSession", message: "configured: playback + mixWithOthers")
        } catch {
            AudioEngineBridge.logError("AudioSession", message: "configuration failed: \(error.localizedDescription)")
        }
    }

    private func removeAudioSessionObservers() {
        audioSessionObservers.forEach { NotificationCenter.default.removeObserver($0) }
        audioSessionObservers.removeAll()
    }

    private func observeAudioSessionEvents() {
        let center = NotificationCenter.default
        let session = AVAudioSession.sharedInstance()
        audioSessionObservers.append(center.addObserver(
            forName: AVAudioSession.interruptionNotification,
            object: session,
            queue: .main
        ) { [weak self] notification in
            self?.handleAudioInterruption(notification)
        })
        audioSessionObservers.append(center.addObserver(
            forName: AVAudioSession.routeChangeNotification,
            object: session,
            queue: .main
        ) { [weak self] _ in
            self?.handleAudioRouteChange()
        })
        // 媒体服务被系统重置（如系统音频守护进程崩溃恢复）后，会话与流都失效，
        // 需重新配置并让桥接按新硬件重建流，否则会一直无声。
        audioSessionObservers.append(center.addObserver(
            forName: AVAudioSession.mediaServicesWereResetNotification,
            object: session,
            queue: .main
        ) { [weak self] _ in
            self?.handleMediaServicesReset()
        })
    }

    private func handleMediaServicesReset() {
        AudioEngineBridge.logInfo("AudioSession", message: "media services were reset — reconfiguring")
        // 重置后旧的观察者可能失效：先全部卸载再重装（幂等）。
        removeAudioSessionObservers()
        AudioEngineBridge.setupLogging()
        configureAudioSession()
        observeAudioSessionEvents()
        // 若正在运行，按新硬件重建 / 重启流（Paused/Idle 时保持不出声）。
        AudioEngineBridge.shared().handleAudioRouteChange()
        RemoteCommandManager.shared.refreshAvailability()
    }

    private func handleAudioRouteChange() {
        do {
            try AVAudioSession.sharedInstance().setActive(true)
        } catch {
            AudioEngineBridge.logError("AudioSession", message: "route reactivation failed: \(error.localizedDescription)")
        }
        AudioEngineBridge.shared().handleAudioRouteChange()
        // 路由变化可能伴随其它音频的出现/消失，刷新锁屏控制权归属。
        RemoteCommandManager.shared.refreshAvailability()
    }

    // MARK: - CallKit（通话状态）

    /// 当前是否有进行中的通话（响铃 / 接通 / 保持）。直接读观测器快照，
    /// 不依赖 delegate 回调时序，避免中断到达时状态过期。
    private var isCallActive: Bool { !callObserver.calls.isEmpty }

    func callObserver(_ callObserver: CXCallObserver, callChanged call: CXCall) {
        let active = !callObserver.calls.isEmpty
        let wasActive = callInProgress
        callInProgress = active
        // 只在「通话结束」边沿（有 → 无）考虑恢复；`.began` 未停用过 driver 时无需处理。
        guard wasActive, !active, suspendedForCall else { return }
        // 正常情况下 `.ended + shouldResume` 会立即恢复，这里延迟一小段再检查：
        // 仅当 `.ended` 始终未到（挂起标记仍在）时才自行恢复兜底 —— 单次、幂等。
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in
            guard let self, self.suspendedForCall, self.suspendedForInterruption else { return }
            self.suspendedForInterruption = false
            self.suspendedForCall = false
            self.fallbackDidResume = true
            AudioEngineBridge.shared().handleAudioInterruptionEndedShouldResume(true)
            AudioEngineBridge.logInfo("AudioSession", message: "call ended: fallback resume (no .ended received)")
            RemoteCommandManager.shared.refreshAvailability()
        }
    }

    // MARK: - Audio Interruption Handling

    private func handleAudioInterruption(_ notification: Notification) {
        guard let info = notification.userInfo,
              let rawType = info[AVAudioSessionInterruptionTypeKey] as? UInt,
              let type = AVAudioSession.InterruptionType(rawValue: rawType) else { return }

        let bridge = AudioEngineBridge.shared()
        switch type {
        case .began:
            // 新一轮中断：复位兜底标记。
            fallbackDidResume = false
            // 仅当节拍器确实在发声时才需要处理；停止 / 暂停态无需重激活。
            let wasAudible = bridge.isRunning && !bridge.isPaused
            if isCallActive {
                // 通话中断：不尝试重激活（mixWithOthers 下可能「假成功」而无声），
                // 直接走既有停流路径，意图保持 Running，待恢复。
                if wasAudible {
                    suspendedForInterruption = true
                    suspendedForCall = true
                }
                bridge.handleAudioInterruptionBegan()
                AudioEngineBridge.logWarn("AudioSession", message: "interruption began during call: driver stopped")
            } else if wasAudible {
                // 非通话（如音乐 App 抢占会话）：先尝试重新并入混音，通常可不停拍。
                do {
                    try AVAudioSession.sharedInstance().setActive(true)
                    AudioEngineBridge.logInfo("AudioSession", message: "interruption began: reactivated, staying in mix")
                } catch {
                    suspendedForInterruption = true
                    bridge.handleAudioInterruptionBegan()
                    AudioEngineBridge.logError("AudioSession", message: "interruption began: reactivation failed (\(error.localizedDescription)), driver stopped")
                }
            }
        case .ended:
            let rawOptions = info[AVAudioSessionInterruptionOptionKey] as? UInt ?? 0
            let shouldResume = AVAudioSession.InterruptionOptions(rawValue: rawOptions)
                    .contains(.shouldResume)
            do {
                try AVAudioSession.sharedInstance().setActive(true)
            } catch {
                AudioEngineBridge.logError("AudioSession", message: "reactivation failed: \(error.localizedDescription)")
            }
            // 中断结束是权威恢复路径；清掉挂起标记，令通话结束兜底不再重复恢复
            // （兜底若已先恢复则此处跳过，避免二次启动 driver）。
            suspendedForInterruption = false
            suspendedForCall = false
            if shouldResume && fallbackDidResume {
                fallbackDidResume = false
                AudioEngineBridge.logInfo("AudioSession", message: "interruption ended: already resumed by call fallback")
            } else {
                fallbackDidResume = false
                bridge.handleAudioInterruptionEndedShouldResume(shouldResume)
            }
        @unknown default:
            break
        }
        // 中断前后其它音频状态可能变化，刷新锁屏控制权归属。
        RemoteCommandManager.shared.refreshAvailability()
    }

    func applicationWillResignActive(_ application: UIApplication) {
        // App 进入后台 — 音频由后台 audio mode 自动维持
    }

    func applicationDidBecomeActive(_ application: UIApplication) {
        // App 回到前台 — 刷新控制中心归属：其它音频在播时让出，无其它音频时夺回。
        RemoteCommandManager.shared.refreshAvailability()
    }
}
