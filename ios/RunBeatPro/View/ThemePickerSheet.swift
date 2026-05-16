import SwiftUI

/// 主题色选择弹窗
struct ThemePickerSheet: View {
    @Environment(AppState.self) private var state
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            List(Array(AppState.ACCENT_COLORS.enumerated()), id: \.offset) { index, color in
                Button {
                    state.accentColorIndex = index
                    dismiss()
                } label: {
                    HStack(spacing: 16) {
                        Circle()
                            .fill(color)
                            .frame(width: 24, height: 24)

                        Text(themeName(at: index))
                            .font(.system(size: 16))
                            .foregroundColor(
                                index == state.accentColorIndex ? color : Color(white: 0.8)
                            )

                        Spacer()

                        if index == state.accentColorIndex {
                            Image(systemName: "checkmark")
                                .foregroundColor(color)
                        }
                    }
                    .padding(.vertical, 4)
                }
            }
            .navigationTitle(localized("dialog_theme_title"))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button(localized("btn_cancel")) { dismiss() }
                }
            }
        }
        .presentationDetents([.medium])
    }

    private func themeName(at index: Int) -> String {
        let names = state.currentLanguage == "zh"
            ? AppState.ACCENT_NAMES_ZH
            : AppState.ACCENT_NAMES_EN
        return names[index]
    }

    private func localized(_ key: String) -> String {
        let table = "Localizable"
        let path = Bundle.main.path(forResource: state.currentLanguage, ofType: "lproj")
        let bundle = path.flatMap { Bundle(path: $0) } ?? .main
        return bundle.localizedString(forKey: key, value: nil, table: table)
    }
}
