import SwiftUI
import Combine

/// 主界面 — 对应 Android activity_main.xml 布局
struct ContentView: View {
    @Environment(AppState.self) private var state

    /// 计时器驱动
    @State private var timerCancellable: Any? = nil
    @State private var showSettings = false

    var body: some View {
        ZStack {
            ScrollView {
                VStack(spacing: 0) {
                    StatusBarView {
                        showSettings = true
                    }

                    Divider()
                        .background(Color(white: 0.15))
                        .padding(.bottom, 8)

                    BPMDisplayView()
                        .padding(.bottom, 4)

                    TimerView()
                        .padding(.bottom, 12)

                    BPMControlsView()
                        .padding(.bottom, 12)

                    StartPauseButton()
                        .padding(.bottom, 12)

                    Divider()
                        .background(Color(white: 0.15))
                        .padding(.bottom, 8)

                    VolumeRow()
                        .padding(.bottom, 8)
                }
                .padding(.horizontal, 16)
                .padding(.vertical, 12)
            }
            .background(Color.background)
            .accessibilityHidden(showSettings)

            SettingsDrawer(isPresented: $showSettings)
                .zIndex(1)
        }
        .overlay(alignment: .leading) { edgeSwipeArea }
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

    // MARK: - 左边缘滑出

    private var edgeSwipeArea: some View {
        Rectangle()
            .fill(Color.clear)
            .frame(width: 16)
            .contentShape(Rectangle())
            .gesture(
                DragGesture(minimumDistance: 20)
                    .onEnded { value in
                        if value.translation.width > 60 {
                            showSettings = true
                        }
                    }
            )
            .allowsHitTesting(!showSettings)
    }

    // MARK: - Timer

    private func startTimer() {
        let timer = Timer.publish(every: 1, on: .main, in: .common).autoconnect()
        timerCancellable = timer.sink { [weak state = state] _ in
            guard let state = state else { return }
            if state.isRunningNotPaused {
                state.elapsedSeconds += 1
            }
            // 系统没有「其它 App 开始/停止播放」通知，借 1s 计时器低频探测，
            // 及时让出 / 夺回锁屏控制权（幂等、轻量）。
            state.refreshRemoteControlAvailability()
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
    static let textSecondary = Color(white: 0.71)  // #B5B5B5
    static let textTimer = Color(white: 0.88)  // #E0E0E0
    static let accent = Color(red: 1.0, green: 0.42, blue: 0.21)  // #FF6B35
    static let controlBg = Color(red: 42/255, green: 42/255, blue: 42/255)  // #2A2A2A
}
