import SwiftUI

/// START / PAUSE / RESUME 控制按钮
struct StartPauseButton: View {
    @Environment(AppState.self) private var state

    var body: some View {
        Button {
            performAction()
            HapticManager.tap()
        } label: {
            Text(buttonLabel)
                .font(.system(size: 18, weight: .semibold))
                .foregroundColor(.textPrimary)
                .frame(maxWidth: .infinity)
                .frame(height: 60)
                .background(
                    RoundedRectangle(cornerRadius: 12)
                        .fill(state.accentColor)
                )
        }
    }

    private var buttonLabel: String {
        if !state.isRunning {
            return localized("btn_start")
        } else if state.isPaused {
            return localized("btn_resume")
        } else {
            return localized("btn_pause")
        }
    }

    private func performAction() {
        if !state.isRunning {
            state.start()
        } else if state.isPaused {
            state.resume()
        } else {
            state.pause()
        }
    }

    private func localized(_ key: String) -> String {
        let table = "Localizable"
        let path = Bundle.main.path(forResource: state.currentLanguage, ofType: "lproj")
        let bundle = path.flatMap { Bundle(path: $0) } ?? .main
        return bundle.localizedString(forKey: key, value: nil, table: table)
    }
}
