import SwiftUI

/// 重音模式选择弹窗
struct AccentPickerSheet: View {
    @Environment(AppState.self) private var state
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            List(Array(AppState.ACCENT_MODE_NAMES_ZH.enumerated()), id: \.offset) { index, _ in
                Button {
                    state.accentMode = index
                    dismiss()
                } label: {
                    HStack {
                        Text(modeName(at: index))
                            .font(.system(size: 16))
                            .foregroundColor(
                                index == state.accentMode ? state.accentColor : Color(white: 0.8)
                            )

                        Spacer()

                        if index == state.accentMode {
                            Image(systemName: "checkmark")
                                .foregroundColor(state.accentColor)
                        }
                    }
                    .padding(.vertical, 4)
                }
            }
            .navigationTitle(localized("dialog_accent_title"))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button(localized("btn_cancel")) { dismiss() }
                }
            }
        }
        .presentationDetents([.medium])
    }

    private func modeName(at index: Int) -> String {
        let names = state.currentLanguage == "zh"
            ? AppState.ACCENT_MODE_NAMES_ZH
            : AppState.ACCENT_MODE_NAMES_EN
        return names[index]
    }

    private func localized(_ key: String) -> String {
        let table = "Localizable"
        let path = Bundle.main.path(forResource: state.currentLanguage, ofType: "lproj")
        let bundle = path.flatMap { Bundle(path: $0) } ?? .main
        return bundle.localizedString(forKey: key, value: nil, table: table)
    }
}
