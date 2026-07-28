import AVFoundation
import Foundation
import NitroModules
import UIKit

/// Backing view. `layerClass` makes the display layer *be* the view's layer, so
/// there is no second layer to keep in sync with bounds — resizing is handled by
/// UIKit rather than by mirroring frames in `layoutSubviews`.
final class WebmPlayerLayerView: UIView {
  override class var layerClass: AnyClass {
    AVSampleBufferDisplayLayer.self
  }

  var displayLayer: AVSampleBufferDisplayLayer {
    // Safe by construction: layerClass above guarantees the type.
    layer as! AVSampleBufferDisplayLayer
  }
}

/// Presents a player's decoded video.
///
/// The engine is reached by downcasting the `player` prop to the concrete
/// implementation. The prop carries the spec protocol, which deliberately cannot
/// express an `AVSampleBufferDisplayLayer` — that type has no meaning on Android.
final class HybridWebmPlayerView: HybridWebmPlayerViewSpec {
  private let container = WebmPlayerLayerView()

  var view: UIView { container }

  var player: (any HybridWebmPlayerSpec)? {
    didSet {
      // Detach from whatever held the layer before, so a reassigned prop cannot
      // leave two engines enqueuing onto the same surface.
      (oldValue as? HybridWebmPlayer)?.detachDisplayLayer(container.displayLayer)
      (player as? HybridWebmPlayer)?.attachDisplayLayer(container.displayLayer)
    }
  }

  var scaleMode: WebmScaleMode = .contain {
    didSet { applyScaleMode() }
  }

  override init() {
    super.init()
    applyScaleMode()
  }

  deinit {
    (player as? HybridWebmPlayer)?.detachDisplayLayer(container.displayLayer)
  }

  private func applyScaleMode() {
    switch scaleMode {
    case .contain: container.displayLayer.videoGravity = .resizeAspect
    case .cover: container.displayLayer.videoGravity = .resizeAspectFill
    case .stretch: container.displayLayer.videoGravity = .resize
    }
  }
}
