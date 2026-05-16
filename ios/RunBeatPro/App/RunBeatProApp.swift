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
                    // 初始化音频引擎
                    AudioEngineBridge.shared().initializeEngine()
                }
        }
    }
}
