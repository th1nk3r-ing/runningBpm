import SwiftUI

/// 音色选择弹窗
struct TimbrePickerSheet: View {
    @Environment(AppState.self) private var state
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            List(Array(AppState.TIMBRE_NAMES_ZH.enumerated()), id: \.offset) { index, _ in
                Button {
                    state.loadTimbre(at: index)
                    dismiss()
                } label: {
                    HStack {
                        Text(timbreName(at: index))
                            .font(.system(size: 16))
                            .foregroundColor(
                                index == state.timbreIndex ? state.accentColor : Color(white: 0.8)
                            )

                        Spacer()

                        if index == state.timbreIndex {
                            Image(systemName: "checkmark")
                                .foregroundColor(state.accentColor)
                        }
                    }
                    .padding(.vertical, 4)
                }
            }
            .navigationTitle(localized("dialog_timbre_title"))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button(localized("btn_cancel")) { dismiss() }
                }
            }
        }
        .presentationDetents([.medium])
    }

    private func timbreName(at index: Int) -> String {
        let names = state.currentLanguage == "zh"
            ? AppState.TIMBRE_NAMES_ZH
            : AppState.TIMBRE_NAMES_EN
        return names[index]
    }

    private func localized(_ key: String) -> String {
        let table = "Localizable"
        let path = Bundle.main.path(forResource: state.currentLanguage, ofType: "lproj")
        let bundle = path.flatMap { Bundle(path: $0) } ?? .main
        return bundle.localizedString(forKey: key, value: nil, table: table)
    }
}
