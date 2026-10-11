import UIKit
import AVFoundation

/// App 生命周期代理 — 负责提前配置 AudioSession
class AppDelegate: NSObject, UIApplicationDelegate {

    private var audioSessionObservers: [NSObjectProtocol] = []

    func application(
        _ application: UIApplication,
        didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil
    ) -> Bool {
        configureAudioSession()
        observeAudioSessionEvents()
        return true
    }

    deinit {
        audioSessionObservers.forEach { NotificationCenter.default.removeObserver($0) }
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
            print("[AudioSession] configured: playback + mixWithOthers")
        } catch {
            print("[AudioSession] configuration failed: \(error.localizedDescription)")
        }
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
    }

    private func handleAudioRouteChange() {
        do {
            try AVAudioSession.sharedInstance().setActive(true)
        } catch {
            print("[AudioSession] route reactivation failed: \(error.localizedDescription)")
        }
        AudioEngineBridge.shared().handleAudioRouteChange()
    }

    private func handleAudioInterruption(_ notification: Notification) {
        guard let info = notification.userInfo,
              let rawType = info[AVAudioSessionInterruptionTypeKey] as? UInt,
              let type = AVAudioSession.InterruptionType(rawValue: rawType) else { return }

        let bridge = AudioEngineBridge.shared()
        switch type {
        case .began:
            bridge.handleAudioInterruptionBegan()
        case .ended:
            let rawOptions = info[AVAudioSessionInterruptionOptionKey] as? UInt ?? 0
            let shouldResume = AVAudioSession.InterruptionOptions(rawValue: rawOptions)
                    .contains(.shouldResume)
            do {
                try AVAudioSession.sharedInstance().setActive(true)
            } catch {
                print("[AudioSession] reactivation failed: \(error.localizedDescription)")
            }
            bridge.handleAudioInterruptionEndedShouldResume(shouldResume)
        @unknown default:
            break
        }
    }

    // MARK: - Audio Interruption Handling

    func applicationWillResignActive(_ application: UIApplication) {
        // App 进入后台 — 音频由后台 audio mode 自动维持
    }

    func applicationDidBecomeActive(_ application: UIApplication) {
        // App 回到前台
    }
}
