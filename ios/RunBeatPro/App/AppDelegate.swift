import UIKit
import AVFoundation

/// App 生命周期代理 — 负责提前配置 AudioSession
class AppDelegate: NSObject, UIApplicationDelegate {

    func application(
        _ application: UIApplication,
        didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil
    ) -> Bool {
        configureAudioSession()
        return true
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

    // MARK: - Audio Interruption Handling

    func applicationWillResignActive(_ application: UIApplication) {
        // App 进入后台 — 音频由后台 audio mode 自动维持
    }

    func applicationDidBecomeActive(_ application: UIApplication) {
        // App 回到前台
    }
}
