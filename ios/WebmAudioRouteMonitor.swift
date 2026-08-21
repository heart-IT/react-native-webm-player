import AVFoundation
import Foundation

/// Reports where audio is currently coming out, and when that changes.
///
/// Read-only by design. Forcing output requires `overrideOutputAudioPort`, which
/// the SDK documents as "only valid for a session using PlayAndRecord category"
/// — adopting that category on a receive-only player would prompt for
/// microphone access it never uses. Output selection stays with the user.
final class WebmAudioRouteMonitor {
  private var onChange: ((WebmAudioRoute) -> Void)?
  private var observer: NSObjectProtocol?

  deinit {
    if let observer { NotificationCenter.default.removeObserver(observer) }
  }

  var currentRoute: WebmAudioRoute {
    Self.route(from: AVAudioSession.sharedInstance().currentRoute)
  }

  /// The routes the system reports.
  ///
  /// iOS exposes the *active* route only — there is no public API enumerating
  /// inactive outputs (Apple's answer is `AVRoutePickerView`). Android can list
  /// every connected output, so this returns more entries there. Documented
  /// rather than papered over.
  var availableRoutes: [WebmAudioRoute] {
    let outputs = AVAudioSession.sharedInstance().currentRoute.outputs
    var seen: [WebmAudioRoute] = []
    for output in outputs {
      let route = Self.route(from: output.portType)
      if !seen.contains(route) { seen.append(route) }
    }
    return seen.isEmpty ? [.speaker] : seen
  }

  func setCallback(_ callback: @escaping (WebmAudioRoute) -> Void) {
    onChange = callback
    if observer == nil {
      observer = NotificationCenter.default.addObserver(
        forName: AVAudioSession.routeChangeNotification,
        object: AVAudioSession.sharedInstance(),
        queue: .main
      ) { [weak self] _ in
        guard let self else { return }
        self.onChange?(self.currentRoute)
      }
    }
  }

  private static func route(from description: AVAudioSessionRouteDescription) -> WebmAudioRoute {
    guard let first = description.outputs.first else { return .unknown }
    return route(from: first.portType)
  }

  private static func route(from portType: AVAudioSession.Port) -> WebmAudioRoute {
    switch portType {
    case .builtInReceiver: return .earpiece
    case .builtInSpeaker: return .speaker
    case .headphones: return .wiredheadset
    case .bluetoothA2DP, .bluetoothLE, .bluetoothHFP: return .bluetooth
    case .usbAudio: return .usb
    default: return .unknown
    }
  }
}
