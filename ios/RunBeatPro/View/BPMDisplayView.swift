import SwiftUI

/// BPM 数字 + 标签
struct BPMDisplayView: View {
    @Environment(AppState.self) private var state
    @State private var scale: CGFloat = 1.0

    var body: some View {
        VStack(spacing: 0) {
            Text(state.bpmDisplay)
                .font(.system(size: 120, weight: .thin, design: .default))
                .foregroundColor(state.accentColor)
                .scaleEffect(scale)
                .frame(maxWidth: .infinity)
                .padding(.vertical, -8)

            Text("BPM")
                .font(.system(size: 18, weight: .regular))
                .foregroundColor(state.accentColor.opacity(0.7))
                .shadow(color: .white.opacity(0.3), radius: 2)
        }
        .onChange(of: state.bpm) { _, _ in
            // 缩放弹跳动画
            withAnimation(.spring(response: 0.2, dampingFraction: 0.5)) {
                scale = 1.25
            }
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) {
                withAnimation(.spring(response: 0.2, dampingFraction: 0.5)) {
                    scale = 1.0
                }
            }
        }
    }
}
