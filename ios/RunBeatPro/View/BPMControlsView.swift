import SwiftUI

/// BPM 调节按钮 [-5] [-1] [+1] [+5]
struct BPMControlsView: View {
    @Environment(AppState.self) private var state

    /// 长按连续调节定时器
    @State private var holdTimer: Timer?

    var body: some View {
        HStack(spacing: 4) {
            bpmButton(delta: -5)
            bpmButton(delta: -1)
            bpmButton(delta: +1)
            bpmButton(delta: +5)
        }
        .disabled(state.isLocked)
        .opacity(state.isLocked ? 0.6 : 1.0)
    }

    @ViewBuilder
    private func bpmButton(delta: Int) -> some View {
        Button {
            stopHold()
            state.adjustBpm(delta: delta)
            HapticManager.tap()
        } label: {
            Text(delta > 0 ? "+\(delta)" : "\(delta)")
                .font(.system(size: 16, weight: .medium))
                .foregroundColor(.textPrimary)
                .frame(maxWidth: .infinity, minHeight: 56)
                .background(
                    RoundedRectangle(cornerRadius: 8)
                        .stroke(state.accentColor, lineWidth: 1)
                        .background(Color.controlBg.cornerRadius(8))
                )
        }
        .simultaneousGesture(
            LongPressGesture(minimumDuration: 0.4)
                .onEnded { _ in
                    startHold(delta: delta)
                }
        )
    }

    // MARK: - 长按连续调节

    private func startHold(delta: Int) {
        holdTimer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { _ in
            state.adjustBpm(delta: delta)
        }
    }

    private func stopHold() {
        holdTimer?.invalidate()
        holdTimer = nil
    }
}
