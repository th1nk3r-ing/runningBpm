import SwiftUI

/// 音色选择行
struct TimbreRow: View {
    @Environment(AppState.self) private var state
    @State private var showPicker = false

    var body: some View {
        Button {
            showPicker = true
        } label: {
            HStack(spacing: 0) {
                Text(localized("label_timbre"))
                    .font(.system(size: 14))
                    .foregroundColor(.textSecondary)

                Spacer()

                Text(state.timbreName)
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
            TimbrePickerSheet()
        }
    }

    private func localized(_ key: String) -> String {
        let table = "Localizable"
        let path = Bundle.main.path(forResource: state.currentLanguage, ofType: "lproj")
        let bundle = path.flatMap { Bundle(path: $0) } ?? .main
        return bundle.localizedString(forKey: key, value: nil, table: table)
    }
}
