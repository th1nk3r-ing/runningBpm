import SwiftUI

/// 状态栏：语言切换 + 主题色选择 + 运行状态 + 锁定按钮
struct StatusBarView: View {
    @Environment(AppState.self) private var state
    @State private var showThemeSheet = false
    @State private var unlockProgress: CGFloat = 0.0
    @State private var isUnlocking = false

    private let unlockDuration: TimeInterval = 2.0

    var body: some View {
        HStack(spacing: 0) {
            // 语言切换
            Button {
                toggleLanguage()
            } label: {
                Text(state.currentLanguage == "zh" ? "EN" : "中文")
                    .font(.system(size: 14, weight: .bold))
                    .foregroundColor(.textSecondary)
                    .padding(8)
            }
            .padding(.trailing, 8)

            // 主题色选择
            Button {
                showThemeSheet = true
            } label: {
                HStack(spacing: 6) {
                    Circle()
                        .fill(state.accentColor)
                        .frame(width: 20, height: 20)
                    Text(state.accentColorName)
                        .font(.system(size: 12))
                        .foregroundColor(state.accentColor)
                }
                .padding(4)
            }
            .padding(.trailing, 12)
            .sheet(isPresented: $showThemeSheet) {
                ThemePickerSheet()
            }

            // 状态圆点
            Circle()
                .fill(state.isRunningNotPaused ? Color.green : Color.gray)
                .frame(width: 10, height: 10)
                .padding(.trailing, 8)

            // 状态文字
            Text(statusText)
                .font(.system(size: 15, weight: .regular))
                .foregroundColor(.textPrimary)
                .frame(maxWidth: .infinity, alignment: .leading)

            // 锁定按钮
            lockButton
        }
    }

    // MARK: - Status Text

    private var statusText: String {
        if !state.isRunning {
            return localized("status_stopped")
        } else if state.isPaused {
            return localized("status_paused")
        } else {
            return localized("status_running")
        }
    }

    // MARK: - Lock Button

    private var lockButton: some View {
        ZStack {
            // 解锁进度环
            if isUnlocking {
                Circle()
                    .stroke(Color.controlBg, lineWidth: 3)
                    .frame(width: 62, height: 62)
                Circle()
                    .trim(from: 0, to: unlockProgress)
                    .stroke(state.accentColor, lineWidth: 3)
                    .frame(width: 62, height: 62)
                    .rotationEffect(.degrees(-90))
            }

            Image(systemName: state.isLocked ? "lock.fill" : "lock.open")
                .font(.system(size: 20))
                .foregroundColor(state.isLocked ? .textSecondary : state.accentColor)
                .frame(width: 50, height: 50)
        }
        .frame(width: 72, height: 72)
        .contentShape(Rectangle())
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { _ in
                    if state.isLocked {
                        startUnlock()
                    } else {
                        // 短按锁定
                        state.isLocked = true
                        HapticManager.lockFeedback()
                    }
                }
                .onEnded { _ in
                    if isUnlocking {
                        cancelUnlock()
                    }
                }
        )
    }

    // MARK: - Unlock Logic（长按 2s 解锁）

    private func startUnlock() {
        guard !isUnlocking else { return }
        isUnlocking = true
        unlockProgress = 0.0

        withAnimation(.linear(duration: unlockDuration)) {
            unlockProgress = 1.0
        }

        // 2s 后解锁
        DispatchQueue.main.asyncAfter(deadline: .now() + unlockDuration) { [weak state = state] in
            guard let state = state, self.isUnlocking else { return }
            state.isLocked = false
            self.isUnlocking = false
            self.unlockProgress = 0.0
            HapticManager.lockFeedback()

            // 解锁后若运行中，重新启动自动锁定
            if state.isRunningNotPaused {
                state.scheduleAutoLock()
            }
        }
    }

    private func cancelUnlock() {
        isUnlocking = false
        unlockProgress = 0.0
    }

    // MARK: - Language

    private func toggleLanguage() {
        state.currentLanguage = state.currentLanguage == "zh" ? "en" : "zh"
        // 更新 UserDefaults
        UserDefaults.standard.setValue(state.currentLanguage, forKey: "language")
    }

    // MARK: - Localization Helper

    private func localized(_ key: String) -> String {
        let table = "Localizable"
        let path = Bundle.main.path(forResource: state.currentLanguage, ofType: "lproj")
        let bundle = path.flatMap { Bundle(path: $0) } ?? .main
        return bundle.localizedString(forKey: key, value: nil, table: table)
    }
}
