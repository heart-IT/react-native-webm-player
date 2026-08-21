package com.margelo.nitro.webmplayer

import android.content.Context
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Handler

/**
 * Reports where audio is currently coming out, and when that changes.
 *
 * Read-only by design, matching iOS. `ExoPlayer.setPreferredAudioDevice` would
 * let Android force an output, but the equivalent on iOS needs the PlayAndRecord
 * category and so a microphone prompt on a player that never records. Rather
 * than let the two platforms diverge, output selection stays with the user.
 */
class AudioRouteMonitor(context: Context, private val handler: Handler) {

  private val audioManager =
    context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
  // Written from the JS thread (setCallback), read on the handler's thread.
  @Volatile private var onChange: ((WebmAudioRoute) -> Unit)? = null
  private var deviceCallback: AudioDeviceCallback? = null

  /**
   * Unlike iOS, this enumerates every connected output rather than only the
   * active one — Android exposes the full list and withholding it would be
   * pretending the platform is less capable than it is.
   */
  fun availableRoutes(): List<WebmAudioRoute> =
    audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
      .map { routeOf(it.type) }
      .filter { it != WebmAudioRoute.UNKNOWN }
      .distinct()
      .ifEmpty { listOf(WebmAudioRoute.SPEAKER) }

  /**
   * The route the system would use now. Android has no single "current output"
   * accessor for media, so this is the highest-priority connected output —
   * the order the platform itself prefers when several are attached.
   */
  fun currentRoute(): WebmAudioRoute {
    val routes = availableRoutes()
    return PRIORITY.firstOrNull { it in routes } ?: routes.firstOrNull()
      ?: WebmAudioRoute.UNKNOWN
  }

  fun setCallback(callback: (WebmAudioRoute) -> Unit) {
    onChange = callback
    if (deviceCallback != null) return
    val cb = object : AudioDeviceCallback() {
      override fun onAudioDevicesAdded(addedDevices: Array<out AudioDeviceInfo>?) {
        onChange?.invoke(currentRoute())
      }

      override fun onAudioDevicesRemoved(removedDevices: Array<out AudioDeviceInfo>?) {
        onChange?.invoke(currentRoute())
      }
    }
    deviceCallback = cb
    audioManager.registerAudioDeviceCallback(cb, handler)
  }

  fun release() {
    deviceCallback?.let { audioManager.unregisterAudioDeviceCallback(it) }
    deviceCallback = null
    onChange = null
  }

  private fun routeOf(type: Int): WebmAudioRoute = when (type) {
    AudioDeviceInfo.TYPE_BUILTIN_EARPIECE -> WebmAudioRoute.EARPIECE
    AudioDeviceInfo.TYPE_BUILTIN_SPEAKER,
    AudioDeviceInfo.TYPE_BUILTIN_SPEAKER_SAFE -> WebmAudioRoute.SPEAKER
    AudioDeviceInfo.TYPE_WIRED_HEADSET,
    AudioDeviceInfo.TYPE_WIRED_HEADPHONES -> WebmAudioRoute.WIREDHEADSET
    AudioDeviceInfo.TYPE_BLUETOOTH_A2DP,
    AudioDeviceInfo.TYPE_BLUETOOTH_SCO -> WebmAudioRoute.BLUETOOTH
    AudioDeviceInfo.TYPE_USB_DEVICE,
    AudioDeviceInfo.TYPE_USB_HEADSET,
    AudioDeviceInfo.TYPE_USB_ACCESSORY -> WebmAudioRoute.USB
    else -> WebmAudioRoute.UNKNOWN
  }

  private companion object {
    /** The order Android itself prefers when several outputs are connected. */
    val PRIORITY = listOf(
      WebmAudioRoute.BLUETOOTH,
      WebmAudioRoute.USB,
      WebmAudioRoute.WIREDHEADSET,
      WebmAudioRoute.EARPIECE,
      WebmAudioRoute.SPEAKER,
    )
  }
}
