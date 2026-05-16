import Foundation

/// 偏好设置管理器 — 封装 UserDefaults（对标 Android PreferencesManager）
/// 所有持久化操作委托给 AppState 的 didSet，此处提供辅助方法
struct PreferencesManager {

    // MARK: - Keys

    private enum Key {
        static let bpmBits       = "bpm_bits"
        static let tickVolume    = "tick_volume"
        static let accent        = "accent"
        static let timbre        = "timbre"
        static let gainLevel     = "gain_level"
        static let accentIndex   = "accent_index"
        static let language      = "language"
    }

    private let ud = UserDefaults.standard

    // MARK: - BPM

    var bpm: Double {
        get {
            let v = ud.double(forKey: Key.bpmBits)
            return v > 0 ? v : 180.0
        }
        set {
            ud.set(newValue, forKey: Key.bpmBits)
        }
    }

    // MARK: - Tick Volume

    var tickVolume: Int {
        get { ud.integer(forKey: Key.tickVolume) }
        set { ud.setValue(newValue, forKey: Key.tickVolume) }
    }

    // MARK: - Accent

    var accentEnabled: Bool {
        get { ud.object(forKey: Key.accent) as? Bool ?? true }
        set { ud.setValue(newValue, forKey: Key.accent) }
    }

    // MARK: - Timbre

    var timbreIndex: Int {
        get { ud.integer(forKey: Key.timbre) }
        set { ud.setValue(newValue, forKey: Key.timbre) }
    }

    // MARK: - Gain Level

    var gainLevel: Int {
        get {
            let v = ud.integer(forKey: Key.gainLevel)
            return (v >= 1 && v <= 3) ? v : 1
        }
        set { ud.setValue(newValue, forKey: Key.gainLevel) }
    }

    // MARK: - Accent Color Index

    var accentColorIndex: Int {
        get {
            let v = ud.integer(forKey: Key.accentIndex)
            return (v >= 0 && v < 5) ? v : 0
        }
        set { ud.setValue(newValue, forKey: Key.accentIndex) }
    }

    // MARK: - Language

    var language: String {
        get { ud.string(forKey: Key.language) ?? "zh" }
        set { ud.setValue(newValue, forKey: Key.language) }
    }
}
