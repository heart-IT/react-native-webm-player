package com.margelo.nitro.webmplayer

import android.content.Context
import android.view.View
import androidx.media3.common.util.UnstableApi
import androidx.media3.ui.AspectRatioFrameLayout
import androidx.media3.ui.PlayerView

/**
 * Presents a player's decoded video.
 *
 * The ExoPlayer instance is reached by downcasting the `player` prop to the
 * concrete implementation. The prop carries the spec type, which deliberately
 * cannot express a `PlayerView` — that has no meaning on iOS.
 *
 * The predecessor kept a global map of React tags to views and required JS to
 * call `attachToView(tag)`. Here the association is the prop itself, so it
 * cannot outlive the view or bind to the wrong player.
 */
@UnstableApi
class HybridWebmPlayerView(context: Context) : HybridWebmPlayerViewSpec() {

  private val playerView: PlayerView =
    PlayerView(context).apply {
      useController = false
      setShowBuffering(PlayerView.SHOW_BUFFERING_ALWAYS)
      // Not setUseArtwork(false): media3 1.8.0 inverts it, mapping false to
      // ARTWORK_DISPLAY_MODE_FIT, so the deprecated call switched artwork on.
      setArtworkDisplayMode(PlayerView.ARTWORK_DISPLAY_MODE_OFF)
      setShutterBackgroundColor(SHUTTER_COLOR)
    }

  override val view: View
    get() = playerView

  override var player: HybridWebmPlayerSpec? = null
    set(value) {
      // Detach the previous player first, so a reassigned prop cannot leave two
      // players bound to the same surface.
      (field as? HybridWebmPlayer)?.detachSurface(playerView)
      field = value
      (value as? HybridWebmPlayer)?.attachSurface(playerView)
    }

  override var scaleMode: WebmScaleMode = WebmScaleMode.CONTAIN
    set(value) {
      field = value
      playerView.resizeMode = when (value) {
        WebmScaleMode.CONTAIN -> AspectRatioFrameLayout.RESIZE_MODE_FIT
        WebmScaleMode.COVER -> AspectRatioFrameLayout.RESIZE_MODE_ZOOM
        WebmScaleMode.STRETCH -> AspectRatioFrameLayout.RESIZE_MODE_FILL
      }
    }

  private companion object {
    const val SHUTTER_COLOR = 0xFF000000.toInt()
  }
}
