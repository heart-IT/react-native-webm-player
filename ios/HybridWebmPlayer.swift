import AVFoundation
import Foundation
import NitroModules

/// Nitro facade over `WebmPlaybackEngine`.
///
/// Deliberately thin: it holds no playback state of its own, so there is nothing
/// here that can disagree with the engine. Its one real job is the ArrayBuffer
/// hand-off in `feedData`.
final class HybridWebmPlayer: HybridWebmPlayerSpec {
  private let engine = WebmPlaybackEngine()

  // MARK: - Lifecycle

  func start() throws -> Bool { engine.start() }
  func stop() throws -> Bool { engine.stop() }
  func pause() throws -> Bool { engine.pause() }
  func resume() throws -> Bool { engine.resume() }

  var isRunning: Bool { engine.isRunning() }
  var isPaused: Bool { engine.isPaused() }

  var playbackState: WebmPlaybackState {
    switch engine.playbackState() {
    case .idle: return .idle
    case .buffering: return .buffering
    case .playing: return .playing
    case .paused: return .paused
    case .failed: return .failed
    @unknown default: return .idle
    }
  }

  // MARK: - Stream data

  func feedData(data: ArrayBuffer) throws -> Bool {
    // A JS-created ArrayBuffer is non-owning: its memory is only valid for the
    // duration of this synchronous call. The engine copies into the ring before
    // returning, so nothing outlives the buffer.
    let size = data.size
    if size == 0 { return false }
    return engine.feedData(data.data, length: size) == size
  }

  func setEndOfStream() throws {
    _ = engine.setEndOfStream()
  }

  func resetStream() throws {
    _ = engine.resetStream()
  }

  // MARK: - Controls

  var muted: Bool {
    get { engine.metrics().muted }
    set { _ = engine.setMuted(newValue) }
  }

  var gain: Double {
    get { Double(engine.metrics().gain) }
    set { _ = engine.setGain(Float(newValue)) }
  }

  var playbackRate: Double {
    get { engine.metrics().playbackRate }
    set { _ = engine.setPlaybackRate(Float(newValue)) }
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

  // MARK: - Metrics

  func getMetrics() throws -> WebmPlayerMetrics {
    let m = engine.metrics()
    return WebmPlayerMetrics(
      bytesFedTotal: Double(m.bytesFedTotal),
      audioPacketsDecoded: Double(m.audioPacketsDecoded),
      videoPacketsDecoded: Double(m.videoPacketsDecoded),
      audioUnderruns: Double(m.audioUnderruns),
      videoFramesDropped: Double(m.videoFramesDropped),
      videoWidth: Double(m.videoWidth),
      videoHeight: Double(m.videoHeight),
      currentTimeSeconds: m.currentTimeSeconds,
      playbackRate: m.playbackRate,
      muted: m.muted,
      gain: Double(m.gain)
    )
  }
}
