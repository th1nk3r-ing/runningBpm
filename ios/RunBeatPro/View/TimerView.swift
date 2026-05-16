import SwiftUI

/// 计时器显示 + 重置按钮
struct TimerView: View {
    @Environment(AppState.self) private var state

    var body: some View {
        HStack(spacing: 0) {
            // 左侧占位（与右侧重置按钮对称）
            Spacer()
                .frame(width: 44)

            // 计时器
            Text(state.timerDisplay)
                .font(.system(size: 22, design: .monospaced))
                .foregroundColor(.textSecondary)
                .frame(maxWidth: .infinity)

            // 重置按钮
            if state.isRunning {
                Button {
                    state.restart()
                    HapticManager.tap()
                } label: {
                    Text("↻")
                        .font(.system(size: 22))
                        .foregroundColor(state.accentColor)
                        .frame(width: 44, height: 44)
                        .background(
                            RoundedRectangle(cornerRadius: 8)
                                .stroke(state.accentColor.opacity(0.5), lineWidth: 1)
                                .background(Color.controlBg.cornerRadius(8))
                        )
                }
            } else {
                Spacer()
                    .frame(width: 44)
            }
        }
    }
}
