import SwiftUI
import Combine

/// 主界面 — 对应 Android activity_main.xml 布局
struct ContentView: View {
    @Environment(AppState.self) private var state

    /// 计时器驱动
    @State private var timerCancellable: Any? = nil

    var body: some View {
        ScrollView {
            VStack(spacing: 0) {
                // ① 状态栏
                StatusBarView()

                Divider()
                    .background(Color(white: 0.15))
                    .padding(.bottom, 8)

                // ② BPM 核心区
                BPMDisplayView()
                    .padding(.bottom, 4)

                // ③ 计时器 + 重置
                TimerView()
                    .padding(.bottom, 12)

                // ④ BPM 调节按钮
                BPMControlsView()
                    .padding(.bottom, 12)

                // ⑤ 开始/暂停
                StartPauseButton()
                    .padding(.bottom, 12)

                Divider()
                    .background(Color(white: 0.15))
                    .padding(.bottom, 8)

                // ⑥ 音量
                VolumeRow()
                    .padding(.bottom, 8)

                // ⑦ 重音
                AccentRow()

                // ⑧ 音色
                TimbreRow()
                    .padding(.top, 8)
                    .padding(.bottom, 8)

                // ⑨ 版本信息
                Text("v1.0")
                    .foregroundColor(.textSecondary)
                    .font(.system(size: 11))
                    .opacity(0.5)
                    .padding(.top, 4)
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 12)
        }
        .background(Color.background)
        .preferredColorScheme(.dark)
        .onAppear {
            startTimer()
            setupRemoteCommands()
        }
        .onDisappear {
            stopTimer()
        }
        .onChange(of: state.isRunningNotPaused) { _, newValue in
            UIApplication.shared.isIdleTimerDisabled = newValue
        }
    }

    // MARK: - Timer

    private func startTimer() {
        let timer = Timer.publish(every: 1, on: .main, in: .common).autoconnect()
        timerCancellable = timer.sink { [weak state = state] _ in
            guard let state = state else { return }
            if state.isRunningNotPaused {
                state.elapsedSeconds += 1
            }
        }
    }

    private func stopTimer() {
        (timerCancellable as? AnyCancellable)?.cancel()
        timerCancellable = nil
    }

    // MARK: - Remote Commands

    private func setupRemoteCommands() {
        RemoteCommandManager.shared.configure(
            play: { [weak state = state] in
                if state?.isRunning == true, state?.isPaused == true {
                    state?.resume()
                }
            },
            pause: { [weak state = state] in
                if state?.isRunningNotPaused == true {
                    state?.pause()
                }
            },
            toggle: { [weak state = state] in
                guard let state = state else { return }
                if !state.isRunning {
                    state.start()
                } else if state.isPaused {
                    state.resume()
                } else {
                    state.pause()
                }
            }
        )
    }
}

// MARK: - Color 扩展

extension Color {
    static let background = Color(red: 18/255, green: 18/255, blue: 18/255)  // #121212
    static let textPrimary = Color.white
    static let textSecondary = Color(white: 0.533)  // #888888
    static let accent = Color(red: 1.0, green: 0.42, blue: 0.21)  // #FF6B35
    static let controlBg = Color(red: 42/255, green: 42/255, blue: 42/255)  // #2A2A2A
}
