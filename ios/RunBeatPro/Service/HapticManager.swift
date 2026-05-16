import UIKit

/// 触觉反馈管理器 — UIImpactFeedbackGenerator 封装
enum HapticManager {

    private static let rigid = UIImpactFeedbackGenerator(style: .rigid)
    private static let light = UIImpactFeedbackGenerator(style: .light)
    private static let soft  = UIImpactFeedbackGenerator(style: .soft)

    /// 预准备所有 generator，减少首次触觉延迟
    static func prepare() {
        rigid.prepare()
        light.prepare()
        soft.prepare()
    }

    /// BPM 按钮点击（硬触感，确认感强）
    static func tap() {
        rigid.impactOccurred()
    }

    /// 轻触（Slider、Toggle 等）
    static func lightTap() {
        light.impactOccurred()
    }

    /// 锁定/解锁反馈
    static func lockFeedback() {
        soft.impactOccurred()
    }
}
