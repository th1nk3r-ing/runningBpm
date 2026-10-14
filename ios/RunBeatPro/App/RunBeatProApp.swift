import SwiftUI

@main
struct RunBeatProApp: App {
    @UIApplicationDelegateAdaptor(AppDelegate.self) var appDelegate
    @State private var appState = AppState()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environment(appState)
                .preferredColorScheme(.dark)
                .onAppear {
                    // 预准备触觉反馈
                    HapticManager.prepare()
                    // 音频引擎由 AudioEngineBridge.shared()（首访问时）init——
                    // 不在此处二次 initializeEngine：它会覆盖已持久化的音量/accent（3.2）。
                }
        }
    }
}
