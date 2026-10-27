import SwiftUI

/// 重音模式选择行
struct AccentRow: View {
    @Environment(AppState.self) private var state
    @State private var showPicker = false

    var body: some View {
        Button {
            showPicker = true
        } label: {
            HStack(spacing: 0) {
                Text(localized("label_accent"))
                    .font(.system(size: 14))
                    .foregroundColor(.textSecondary)

                Spacer()

                Text(state.accentModeName)
                    .font(.system(size: 14, weight: .bold))
                    .foregroundColor(state.accentColor)

                Text("  ▾")
                    .font(.system(size: 14))
                    .foregroundColor(state.accentColor)
            }
            .padding(.horizontal, 12)
            .frame(height: 52)
            .background(
                RoundedRectangle(cornerRadius: 10)
                    .stroke(state.accentColor.opacity(0.3), lineWidth: 1)
                    .background(Color(white: 0.12).cornerRadius(10))
            )
        }
        .disabled(state.isLocked)
        .opacity(state.isLocked ? 0.4 : 1.0)
        .sheet(isPresented: $showPicker) {
            AccentPickerSheet()
        }
    }

    private func localized(_ key: String) -> String {
        let table = "Localizable"
        let path = Bundle.main.path(forResource: state.currentLanguage, ofType: "lproj")
        let bundle = path.flatMap { Bundle(path: $0) } ?? .main
        return bundle.localizedString(forKey: key, value: nil, table: table)
    }
}

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
