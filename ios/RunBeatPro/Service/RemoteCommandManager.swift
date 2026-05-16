import MediaPlayer
import Foundation

/// 锁屏控制管理器 — MPRemoteCommandCenter + MPNowPlayingInfoCenter
final class RemoteCommandManager {

    static let shared = RemoteCommandManager()

    private let commandCenter = MPRemoteCommandCenter.shared()
    private var onPlay: (() -> Void)?
    private var onPause: (() -> Void)?
    private var onToggle: (() -> Void)?

    private init() {
        setupCommands()
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
    }

    private func setupCommands() {
        // 播放 / 暂停
        commandCenter.playCommand.addTarget { [weak self] _ in
            self?.onPlay?()
            return .success
        }
        commandCenter.pauseCommand.addTarget { [weak self] _ in
            self?.onPause?()
            return .success
        }
        commandCenter.togglePlayPauseCommand.addTarget { [weak self] _ in
            self?.onToggle?()
            return .success
        }
    }

    // MARK: - Now Playing Info

    /// 更新锁屏界面信息
    func updateNowPlaying(bpm: Double, elapsed: Int, isPaused: Bool) {
        var info: [String: Any] = [
            MPMediaItemPropertyTitle: "RunBeat Pro",
            MPMediaItemPropertyArtist: "\(Int(bpm)) BPM · 跑步节拍器",
            MPNowPlayingInfoPropertyElapsedPlaybackTime: TimeInterval(elapsed),
            MPMediaItemPropertyPlaybackDuration: TimeInterval(0),  // 直播/无限时长
            MPNowPlayingInfoPropertyPlaybackRate: isPaused ? 0.0 : 1.0,
        ]

        MPNowPlayingInfoCenter.default().nowPlayingInfo = info
    }

    /// 清空锁屏信息
    func clearNowPlaying() {
        MPNowPlayingInfoCenter.default().nowPlayingInfo = nil
    }
}
