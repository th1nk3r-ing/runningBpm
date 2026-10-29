import SwiftUI

/// 音量滑块 + 倍率按钮
struct VolumeRow: View {
    @Environment(AppState.self) private var state
    @State private var volumeBeforeDrag: Float?

    var body: some View {
        HStack(spacing: 8) {
            Text(localized("label_volume"))
                .font(.system(size: 13))
                .foregroundColor(.textSecondary)
                .frame(width: 56, alignment: .leading)

            Slider(
                value: Binding(
                    get: { Double(state.tickVolume) },
                    set: { state.tickVolume = Float($0) }
                ),
                in: 0...1.0,
                onEditingChanged: { editing in
                    if editing {
                        volumeBeforeDrag = state.tickVolume
                    } else if let before = volumeBeforeDrag, before != state.tickVolume {
                        state.logVolumeCommitted()
                    }
                }
            )
            .tint(state.accentColor)
            .disabled(state.isLocked)
            .opacity(state.isLocked ? 0.6 : 1.0)

            Button {
                state.cycleGain()
                HapticManager.lightTap()
            } label: {
                Text(state.gainLabel)
                    .font(.system(size: 14, weight: .medium))
                    .foregroundColor(state.accentColor)
                    .frame(width: 52, height: 52)
                    .background(
                        RoundedRectangle(cornerRadius: 8)
                            .stroke(state.accentColor, lineWidth: 1)
                            .background(Color.controlBg.cornerRadius(8))
                    )
            }
            .disabled(state.isLocked)
            .opacity(state.isLocked ? 0.6 : 1.0)
        }
    }

    private func localized(_ key: String) -> String {
        let table = "Localizable"
        let path = Bundle.main.path(forResource: state.currentLanguage, ofType: "lproj")
        let bundle = path.flatMap { Bundle(path: $0) } ?? .main
        return bundle.localizedString(forKey: key, value: nil, table: table)
    }
}
