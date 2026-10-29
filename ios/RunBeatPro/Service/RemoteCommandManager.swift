import MediaPlayer
import AVFoundation
import Foundation

/// 锁屏控制管理器 — MPRemoteCommandCenter + MPNowPlayingInfoCenter
///
/// 与其它音频 App 共存：把系统控制中心视为可让出的资源。
/// `isOtherAudioPlaying` 为 true 时清空 NowPlaying 并禁用 play/pause/toggle，
/// 避免劫持锁屏 / 耳机线控；无其它音频时再恢复命令与最近的 NowPlaying 快照。
final class RemoteCommandManager {

    static let shared = RemoteCommandManager()

    private let commandCenter = MPRemoteCommandCenter.shared()
    private var onPlay: (() -> Void)?
    private var onPause: (() -> Void)?
    private var onToggle: (() -> Void)?

    /// 最近一次要发布的锁屏信息快照 — 让出控制权后重新夺回时可立即恢复卡片。
    private var pendingInfo: [String: Any]?
    /// 当前是否已因其它音频在播而让出控制中心（幂等闸门，避免重复开关/刷屏）。
    private var hasYielded = false

    // MARK: - 后台定期探测

    /// 系统没有「其它 App 开始/停止出声」的通知，且 SwiftUI 的 1s 计时器在后台可能停止。
    /// 这里用后台队列上的 DispatchSourceTimer 周期探测（约 2s，带 leeway），
    /// 读 isOtherAudioPlaying 后回主队列走既有幂等的归属逻辑。
    private let timerQueue = DispatchQueue(
        label: "com.runbeat.remotecommand.availability",
        qos: .utility
    )
    private var availabilityTimer: DispatchSourceTimer?
    private let availabilityInterval: TimeInterval = 2.0

    private init() {
        setupCommands()
    }

    deinit {
        stopAvailabilityTimer()
    }

    // MARK: - Setup

    /// 注册锁屏控制命令
    /// - Parameters:
    ///   - onPlay:  继续回调
    ///   - onPause: 暂停回调
    ///   - onToggle: START（停止状态下）
    func configure(
        play: @escaping () -> Void,
        pause: @escaping () -> Void,
        toggle: @escaping () -> Void
    ) {
        onPlay = play
        onPause = pause
        onToggle = toggle
        // 注册后立即按当前音频环境决定控制权归属。
        refreshAvailability()
        // 命令已注册：启动后台定期探测（幂等）。
        startAvailabilityTimer()
    }

    private func setupCommands() {
        // 播放 / 暂停 / 切换
        // 已让出控制权（有其它音频在播）时直接 commandFailed，不触发我们的回调。
        commandCenter.playCommand.addTarget { [weak self] _ in
            guard let self, !self.hasYielded else { return .commandFailed }
            self.onPlay?()
            return .success
        }
        commandCenter.pauseCommand.addTarget { [weak self] _ in
            guard let self, !self.hasYielded else { return .commandFailed }
            self.onPause?()
            return .success
        }
        commandCenter.togglePlayPauseCommand.addTarget { [weak self] _ in
            guard let self, !self.hasYielded else { return .commandFailed }
            self.onToggle?()
            return .success
        }
    }

    // MARK: - 定期探测定时器

    /// 启动后台定期探测（幂等）。命令未注册时不启动。
    private func startAvailabilityTimer() {
        guard availabilityTimer == nil else { return }
        guard onPlay != nil || onPause != nil || onToggle != nil else { return }

        let timer = DispatchSource.makeTimerSource(queue: timerQueue)
        timer.schedule(
            deadline: .now() + availabilityInterval,
            repeating: availabilityInterval,
            leeway: .milliseconds(500)
        )
        timer.setEventHandler { [weak self] in
            guard let self else { return }
            // 读操作线程安全；回主队列走既有的幂等归属逻辑。
            let yield = AVAudioSession.sharedInstance().isOtherAudioPlaying
            DispatchQueue.main.async { [weak self] in
                self?.applyOwnership(yield: yield)
            }
        }
        timer.resume()
        availabilityTimer = timer
    }

    private func stopAvailabilityTimer() {
        availabilityTimer?.setEventHandler {}
        availabilityTimer?.cancel()
        availabilityTimer = nil
    }

    // MARK: - Ownership（控制权归属）

    /// 其它 App 是否正在出声 — 决定是否让出系统控制中心。
    private var isOtherAudioPlaying: Bool {
        AVAudioSession.sharedInstance().isOtherAudioPlaying
    }

    /// 刷新控制权归属。幂等，可在状态变化 / 音频事件 / 周期探测中频繁调用。
    func refreshAvailability() {
        applyOwnership(yield: isOtherAudioPlaying)
    }

    /// 应用控制权归属；仅在归属真正翻转时才改命令开关与 NowPlaying。
    /// 必须在主队列调用（两者均为主线程 UI 资源）。
    private func applyOwnership(yield: Bool) {
        guard yield != hasYielded else {
            // 状态未变：已让出时确保 NowPlaying 保持清空（防止被外部误写）。
            if yield { MPNowPlayingInfoCenter.default().nowPlayingInfo = nil }
            return
        }
        hasYielded = yield
        commandCenter.playCommand.isEnabled = !yield
        commandCenter.pauseCommand.isEnabled = !yield
        commandCenter.togglePlayPauseCommand.isEnabled = !yield
        if yield {
            MPNowPlayingInfoCenter.default().nowPlayingInfo = nil
            AudioEngineBridge.logInfo("RemoteCommand", message: "其它音频在播，让出系统控制中心")
        } else {
            MPNowPlayingInfoCenter.default().nowPlayingInfo = pendingInfo
            AudioEngineBridge.logInfo("RemoteCommand", message: "无其它音频，恢复锁屏控制")
        }
    }

    // MARK: - Now Playing Info

    /// 更新锁屏界面信息；有其它音频在播时改为让出（清空 + 禁用），不发布。
    func updateNowPlaying(bpm: Double, elapsed: Int, isPaused: Bool) {
        let info: [String: Any] = [
            MPMediaItemPropertyTitle: "RunBeat Pro",
            MPMediaItemPropertyArtist: "\(Int(bpm)) BPM · 跑步节拍器",
            MPNowPlayingInfoPropertyElapsedPlaybackTime: TimeInterval(elapsed),
            MPMediaItemPropertyPlaybackDuration: TimeInterval(0),  // 直播/无限时长
            MPNowPlayingInfoPropertyPlaybackRate: isPaused ? 0.0 : 1.0,
        ]
        pendingInfo = info

        if isOtherAudioPlaying {
            applyOwnership(yield: true)
        } else {
            applyOwnership(yield: false)
            MPNowPlayingInfoCenter.default().nowPlayingInfo = info
        }
        // 音频状态变化通常也是「其它音频出现 / 消失」的时机，确保后台探测在跑。
        startAvailabilityTimer()
    }

    /// 清空锁屏信息
    func clearNowPlaying() {
        pendingInfo = nil
        MPNowPlayingInfoCenter.default().nowPlayingInfo = nil
    }
}
