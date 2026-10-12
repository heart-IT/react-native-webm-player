import AVFoundation
import Foundation
import NitroModules

/// Nitro facade over `WebmPlaybackEngine`.
///
/// Deliberately thin: it holds no playback state of its own, so there is nothing
/// here that can disagree with the engine.
final class HybridWebmPlayer: HybridWebmPlayerSpec {
  private let engine = WebmPlaybackEngine()

  // MARK: - Lifecycle

  func play(key: String) throws { engine.play(withKey: key) }
  func stop() throws { engine.stop() }

  var playbackState: WebmPlaybackState {
    switch engine.playbackState() {
    case .idle: return .idle
    case .buffering: return .buffering
    case .playing: return .playing
    case .failed: return .failed
    @unknown default: return .idle
    }
  }

  var muted: Bool {
    get { engine.muted() }
    set { engine.setMuted(newValue) }
  }

  // MARK: - Video surface

  /// Called by `HybridWebmPlayerView`. Not on the spec: the spec is the parity
  /// contract with Android, and an `AVSampleBufferDisplayLayer` has no meaning
  /// there.
  func attachDisplayLayer(_ layer: AVSampleBufferDisplayLayer) {
    engine.setDisplayLayer(layer)
  }

  /// Detaching only clears the engine's layer if it is still the one passed in,
  /// so a view being torn down after another has already attached cannot pull
  /// the surface out from under the newer one.
  func detachDisplayLayer(_ layer: AVSampleBufferDisplayLayer) {
    engine.detachDisplayLayer(layer)
  }

  // MARK: - Health

  func setHealthCallback(callback: @escaping (WebmHealthEvent) -> Void) throws {
    // The engine reports transitions as strings; Nitro marshals the call onto
    // the JS thread itself, so no hop is needed here.
    engine.setHealthCallback { status, detail in
      callback(WebmHealthEvent(status: Self.healthStatus(status), detail: detail))
    }
  }

  private static func healthStatus(_ raw: String) -> WebmHealthStatus {
    switch raw {
    case "playing": return .playing
    case "ended": return .ended
    case "failed": return .failed
    default: return .buffering
    }
  }

  // MARK: - Metrics

  func getMetrics() throws -> WebmPlayerMetrics {
    let m = engine.metrics()
    return WebmPlayerMetrics(
      bytesReceived: Double(m.bytesReceived),
      audioPacketsDecoded: Double(m.audioPacketsDecoded),
      videoPacketsDecoded: Double(m.videoPacketsDecoded),
      videoFramesDropped: Double(m.videoFramesDropped),
      videoWidth: Double(m.videoWidth),
      videoHeight: Double(m.videoHeight),
      currentTimeSeconds: m.currentTimeSeconds
    )
  }
}
