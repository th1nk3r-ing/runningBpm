import SwiftUI

/// 重音开关
struct AccentRow: View {
    @Environment(AppState.self) private var state

    var body: some View {
        HStack(spacing: 0) {
            Text(localized("label_accent"))
                .font(.system(size: 14))
                .foregroundColor(.textSecondary)
                .frame(maxWidth: .infinity, alignment: .leading)

            Toggle("", isOn: Binding(
                get: { state.accentOn },
                set: { state.accentOn = $0 }
            ))
            .tint(state.accentColor)
            .disabled(state.isLocked)
            .opacity(state.isLocked ? 0.4 : 1.0)
        }
        .frame(height: 52)
    }

    private func localized(_ key: String) -> String {
        let table = "Localizable"
        let path = Bundle.main.path(forResource: state.currentLanguage, ofType: "lproj")
        let bundle = path.flatMap { Bundle(path: $0) } ?? .main
        return bundle.localizedString(forKey: key, value: nil, table: table)
    }
}
