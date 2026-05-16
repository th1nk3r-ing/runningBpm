import SwiftUI
import Combine

/// 全局应用状态 — 所有 UI 组件单向依赖此模型
@Observable
final class AppState {

    // MARK: - BPM

    var bpm: Double = 180.0 {
        didSet {
            guard oldValue != bpm else { return }
            AudioEngineBridge.shared().setBpm(bpm)
            scheduleBpmSave()
            updateNowPlaying()
        }
    }

    // MARK: - Engine State

    var isRunning: Bool = false
    var isPaused: Bool = false

    /// Running 且未暂停
    var isRunningNotPaused: Bool { isRunning && !isPaused }

    // MARK: - Timer

    var elapsedSeconds: Int = 0 {
        didSet {
            updateNowPlaying()
        }
    }

    var timerDisplay: String {
        let h = elapsedSeconds / 3600
        let m = (elapsedSeconds % 3600) / 60
        let s = elapsedSeconds % 60
        return String(format: "%02d:%02d:%02d", h, m, s)
    }

    var bpmDisplay: String {
        String(Int(bpm))
    }

    // MARK: - Lock

    var isLocked: Bool = false {
        didSet {
            if isLocked {
                cancelAutoLock()
            }
        }
    }

    // MARK: - Volume & Accent

    var tickVolume: Float = 0.8 {    // 0.0 ~ 1.0
        didSet {
            AudioEngineBridge.shared().setTickVolume(Double(tickVolume))
            saveVolume()
        }
    }

    var accentOn: Bool = true {
        didSet {
            AudioEngineBridge.shared().setAccent(accentOn)
            saveAccent()
        }
    }

    // MARK: - Gain Level (x1/x2/x3)

    var gainLevel: Int = 1 {
        didSet {
            if gainLevel < 1 { gainLevel = 1 }
            if gainLevel > 3 { gainLevel = 3 }
            AudioEngineBridge.shared().setOutputGain(Double(gainLevel))
            saveGainLevel()
        }
    }

    var gainLabel: String { "x\(gainLevel)" }

    // MARK: - Timbre

    var timbreIndex: Int = 0 {
        didSet {
            saveTimbre()
        }
    }

    // MARK: - Theme

    var accentColorIndex: Int = 0 {
        didSet {
            saveAccentIndex()
        }
    }

    var accentColor: Color {
        Self.ACCENT_COLORS[accentColorIndex]
    }

    static let ACCENT_COLORS: [Color] = [
        Color(red: 1.0, green: 0.42, blue: 0.21),     // 橙焰 #FF6B35
        Color(red: 0.0, green: 0.74, blue: 0.83),      // 青电 #00BCD4
        Color(red: 0.61, green: 0.15, blue: 0.69),     // 紫脉 #9C27B0
        Color(red: 0.30, green: 0.69, blue: 0.31),     // 绿野 #4CAF50
        Color(red: 0.88, green: 0.88, blue: 0.88),     // 白月 #E0E0E0
    ]

    static let ACCENT_NAMES_ZH = ["橙焰", "青电", "紫脉", "绿野", "白月"]
    static let ACCENT_NAMES_EN = ["Flame", "Cyan", "Purple", "Green", "White"]

    var accentColorName: String {
        let names = currentLanguage == "zh" ? Self.ACCENT_NAMES_ZH : Self.ACCENT_NAMES_EN
        return names[accentColorIndex]
    }

    // MARK: - Language

    var currentLanguage: String = "zh" {
        didSet {
            saveLanguage()
        }
    }

    // MARK: - Timbre

    static let TIMBRE_PATHS: [(hi: String, lo: String)] = [
        ("sounds/default/tick_hi.wav",       "sounds/default/tick_lo.wav"),
        ("sounds/audios/BassDrum1.wav",      "sounds/audios/BassDrum2.wav"),
        ("sounds/audios/Clap1.wav",          "sounds/audios/Clap2.wav"),
        ("sounds/audios/Claves1.wav",        "sounds/audios/Claves2.wav"),
        ("sounds/audios/Rimshot1.wav",       "sounds/audios/Rimshot2.wav"),
        ("sounds/audios/downbeat.wav",       "sounds/audios/upbeat.wav"),
    ]

    static let TIMBRE_NAMES_ZH = ["默认", "底鼓", "拍手", "响棒", "边击", "强弱拍"]
    static let TIMBRE_NAMES_EN = ["Default", "Bass Drum", "Clap", "Claves", "Rimshot", "Down/Upbeat"]

    var timbreName: String {
        let names = currentLanguage == "zh" ? Self.TIMBRE_NAMES_ZH : Self.TIMBRE_NAMES_EN
        return names[timbreIndex]
    }

    // MARK: - Lifecycle

    init() {
        loadPersistedState()
    }

    // MARK: - Engine Control

    func start() {
        guard !isRunning else { return }
        isRunning = true
        isPaused = false
        elapsedSeconds = 0
        AudioEngineBridge.shared().start(withBpm: bpm)
        RemoteCommandManager.shared.updateNowPlaying(
            bpm: bpm,
            elapsed: elapsedSeconds,
            isPaused: false
        )
        scheduleAutoLock()
    }

    func pause() {
        guard isRunning && !isPaused else { return }
        isPaused = true
        AudioEngineBridge.shared().pause()
        RemoteCommandManager.shared.updateNowPlaying(
            bpm: bpm,
            elapsed: elapsedSeconds,
            isPaused: true
        )
        cancelAutoLock()
    }

    func resume() {
        guard isRunning && isPaused else { return }
        isPaused = false
        AudioEngineBridge.shared().resume()
        RemoteCommandManager.shared.updateNowPlaying(
            bpm: bpm,
            elapsed: elapsedSeconds,
            isPaused: false
        )
        scheduleAutoLock()
    }

    func stop() {
        isRunning = false
        isPaused = false
        elapsedSeconds = 0
        AudioEngineBridge.shared().stop()
        cancelAutoLock()
        isLocked = false
    }

    func restart() {
        AudioEngineBridge.shared().stop()
        elapsedSeconds = 0
        isRunning = true
        isPaused = false
        AudioEngineBridge.shared().start(withBpm: bpm)
        RemoteCommandManager.shared.updateNowPlaying(
            bpm: bpm,
            elapsed: elapsedSeconds,
            isPaused: false
        )
        scheduleAutoLock()
    }

    func adjustBpm(delta: Int) {
        var newBpm = bpm + Double(delta)
        newBpm = max(120.0, min(220.0, newBpm))
        if newBpm != bpm {
            bpm = newBpm
        }
    }

    func cycleGain() {
        gainLevel = gainLevel % 3 + 1
    }

    func loadTimbre(at index: Int) {
        guard index >= 0, index < Self.TIMBRE_PATHS.count else { return }
        timbreIndex = index
        let paths = Self.TIMBRE_PATHS[index]
        let hiURL = Bundle.main.url(forResource: paths.hi.replacingOccurrences(of: ".wav", with: ""),
                                     withExtension: "wav",
                                     subdirectory: "sounds/\(paths.hi.hasPrefix("sounds/") ? "" : "")")
        let loURL = Bundle.main.url(forResource: paths.lo.replacingOccurrences(of: ".wav", with: ""),
                                     withExtension: "wav",
                                     subdirectory: "sounds/\(paths.lo.hasPrefix("sounds/") ? "" : "")")
        // 简化路径处理 — 直接使用 Bundle path
        let hiPath = Bundle.main.path(forResource: (paths.hi as NSString).deletingPathExtension,
                                       ofType: "wav",
                                       inDirectory: nil) ?? Bundle.main.path(forResource: "tick_hi", ofType: "wav")
        let loPath = Bundle.main.path(forResource: (paths.lo as NSString).deletingPathExtension,
                                       ofType: "wav",
                                       inDirectory: nil) ?? Bundle.main.path(forResource: "tick_lo", ofType: "wav")

        if let hiP = hiPath, let loP = loPath {
            AudioEngineBridge.shared().loadTickHi(withFileURL: NSURL.fileURL(withPath: hiP),
                                                   tickLo: NSURL.fileURL(withPath: loP))
        }
    }

    // MARK: - Lock

    private var autoLockWorkItem: DispatchWorkItem?

    func scheduleAutoLock() {
        cancelAutoLock()
        let item = DispatchWorkItem { [weak self] in
            guard let self = self, self.isRunning, !self.isPaused, !self.isLocked else { return }
            self.isLocked = true
        }
        autoLockWorkItem = item
        DispatchQueue.main.asyncAfter(deadline: .now() + 60, execute: item)
    }

    func cancelAutoLock() {
        autoLockWorkItem?.cancel()
        autoLockWorkItem = nil
    }

    // MARK: - NowPlaying

    private func updateNowPlaying() {
        RemoteCommandManager.shared.updateNowPlaying(
            bpm: bpm,
            elapsed: elapsedSeconds,
            isPaused: !isRunning || isPaused
        )
    }

    // MARK: - Persistence

    private func scheduleBpmSave() {
        // 防抖：300ms 后保存
        let key = "bpm"
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [weak self] in
            guard let self = self else { return }
            UserDefaults.standard.set(self.bpm, forKey: key)
        }
    }

    private func saveVolume() {
        UserDefaults.standard.setValue(Int(tickVolume * 100), forKey: "tick_volume")
    }

    private func saveAccent() {
        UserDefaults.standard.setValue(accentOn, forKey: "accent")
    }

    private func saveGainLevel() {
        UserDefaults.standard.setValue(gainLevel, forKey: "gain_level")
    }

    private func saveTimbre() {
        UserDefaults.standard.setValue(timbreIndex, forKey: "timbre")
    }

    private func saveAccentIndex() {
        UserDefaults.standard.setValue(accentColorIndex, forKey: "accent_index")
    }

    private func saveLanguage() {
        UserDefaults.standard.setValue(currentLanguage, forKey: "language")
    }

    private func loadPersistedState() {
        let ud = UserDefaults.standard

        // BPM
        bpm = ud.double(forKey: "bpm")
        if bpm <= 0 { bpm = 180.0 }

        tickVolume = Float(ud.integer(forKey: "tick_volume")) / 100.0
        if tickVolume <= 0 { tickVolume = 0.8 }

        accentOn = ud.object(forKey: "accent") as? Bool ?? true
        gainLevel = ud.integer(forKey: "gain_level")
        if gainLevel < 1 || gainLevel > 3 { gainLevel = 1 }

        let savedTimbre = ud.integer(forKey: "timbre")
        if savedTimbre >= 0 && savedTimbre < Self.TIMBRE_PATHS.count {
            timbreIndex = savedTimbre
        }

        let savedAccentIdx = ud.integer(forKey: "accent_index")
        if savedAccentIdx >= 0 && savedAccentIdx < Self.ACCENT_COLORS.count {
            accentColorIndex = savedAccentIdx
        }

        currentLanguage = ud.string(forKey: "language") ?? "zh"
    }
}
