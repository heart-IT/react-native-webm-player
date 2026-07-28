package com.margelo.nitro.webmplayer

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.os.Build
import android.os.Handler

/**
 * Holds audio focus for the duration of playback and reports what the system
 * wants us to do when another app takes it.
 *
 * Focus changes are delivered on [handler], which is the main looper, because
 * every reaction to them touches ExoPlayer.
 */
class AudioFocusController(context: Context, private val handler: Handler) {

  enum class Change { PAUSE, DUCK, RESTORE }

  private val audioManager =
    context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
  private var request: AudioFocusRequest? = null

  fun acquire(onChange: (Change) -> Unit) {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
    if (request != null) return

    val attrs = AudioAttributes.Builder()
      .setUsage(AudioAttributes.USAGE_MEDIA)
      .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
      .build()
    val built = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
      .setAudioAttributes(attrs)
      .setWillPauseWhenDucked(true)
      .setOnAudioFocusChangeListener({ change ->
        when (change) {
          AudioManager.AUDIOFOCUS_LOSS,
          AudioManager.AUDIOFOCUS_LOSS_TRANSIENT -> onChange(Change.PAUSE)
          AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK -> onChange(Change.DUCK)
          AudioManager.AUDIOFOCUS_GAIN -> onChange(Change.RESTORE)
        }
      }, handler)
      .build()

    request = built
    audioManager.requestAudioFocus(built)
  }

  fun release() {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
    request?.let { audioManager.abandonAudioFocusRequest(it) }
    request = null
  }
}
