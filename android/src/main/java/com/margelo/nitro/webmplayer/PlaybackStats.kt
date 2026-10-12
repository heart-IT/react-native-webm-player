package com.margelo.nitro.webmplayer

import android.os.Handler
import androidx.media3.common.PlaybackException
import androidx.media3.common.Player
import androidx.media3.common.VideoSize
import androidx.media3.common.util.UnstableApi
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.exoplayer.analytics.AnalyticsListener
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong

/**
 * Mirrors main-thread-only ExoPlayer state into atomics.
 *
 * Everything ExoPlayer knows — position, decoder counters, video size, errors —
 * is main-thread-confined, but the Nitro spec's getters are synchronous and
 * called from JS. Reaching into the player from there would be a threading
 * violation, so state is pushed here instead: listeners for events, a polled
 * tick for the counters, which have no callback.
 *
 * Counters are cumulative across sessions, matching iOS.
 */
@UnstableApi
class PlaybackStats(private val mainHandler: Handler) {

  /** Player events that drive session policy. Delivered on the main thread. */
  interface Events {
    fun onHealth(status: WebmHealthStatus, detail: String)
    fun onFirstFrame()
    fun onEnded()
    fun onAudioFocus(held: Boolean)
    /** ExoPlayer gave up on the current source; the player decides whether to rejoin. */
    fun onPlayerError(detail: String)
  }

  val failed = AtomicBoolean(false)
  // Per session: the first audio or video frame actually presented, which is
  // what separates BUFFERING from PLAYING for a live stream.
  val rendered = AtomicBoolean(false)

  private val audioPackets = AtomicLong(0)
  private val videoPackets = AtomicLong(0)
  private val videoDropped = AtomicLong(0)
  private val videoW = AtomicInteger(0)
  private val videoH = AtomicInteger(0)
  private val positionMs = AtomicLong(0)

  private var tick: Runnable? = null

  // Decoder counters restart at zero with every ExoPlayer instance (and every
  // re-prepare after a rejoin); these bases carry the totals accumulated
  // so far, so the atomics stay cumulative. Main thread only, like the poll.
  private var audioPacketsBase = 0L
  private var videoPacketsBase = 0L
  private var videoDroppedBase = 0L

  /** Wires listeners and starts the counter poll. Main thread only. */
  fun attach(exo: ExoPlayer, events: Events) {
    exo.addListener(object : Player.Listener {
      override fun onPlayWhenReadyChanged(value: Boolean, reason: Int) {
        // ExoPlayer reports both the loss-driven pause and the regain-driven
        // resume under this one reason.
        if (reason == Player.PLAY_WHEN_READY_CHANGE_REASON_AUDIO_FOCUS_LOSS) {
          events.onAudioFocus(value)
        }
      }

      override fun onPlaybackStateChanged(state: Int) {
        when (state) {
          Player.STATE_READY -> events.onHealth(WebmHealthStatus.PLAYING, "ready")
          Player.STATE_BUFFERING -> events.onHealth(WebmHealthStatus.BUFFERING, "buffering")
          Player.STATE_ENDED -> events.onEnded()
          else -> Unit
        }
      }

      override fun onPlayerError(error: PlaybackException) = events.onPlayerError(error.errorCodeName)

      override fun onVideoSizeChanged(videoSize: VideoSize) {
        videoW.set(videoSize.width)
        videoH.set(videoSize.height)
      }
    })

    exo.addAnalyticsListener(object : AnalyticsListener {
      override fun onRenderedFirstFrame(
        eventTime: AnalyticsListener.EventTime,
        output: Any,
        renderTimeMs: Long,
      ) = firstFrame(events)

      override fun onAudioPositionAdvancing(
        eventTime: AnalyticsListener.EventTime,
        playoutStartSystemTimeMs: Long,
      ) = firstFrame(events)
    })

    startPolling(exo)
  }

  /** Stops the poll. Main thread only. Safe to call repeatedly. */
  fun detach() {
    tick?.let { mainHandler.removeCallbacks(it) }
    tick = null
  }

  /** Call whenever a new session is prepared: decoders restart, nothing is rendered yet. */
  fun newSession() {
    audioPacketsBase = audioPackets.get()
    videoPacketsBase = videoPackets.get()
    videoDroppedBase = videoDropped.get()
    rendered.set(false)
  }

  private fun firstFrame(events: Events) {
    if (rendered.compareAndSet(false, true)) events.onFirstFrame()
  }

  fun snapshot(bytesReceived: Long) = WebmPlayerMetrics(
    bytesReceived = bytesReceived.toDouble(),
    audioPacketsDecoded = audioPackets.get().toDouble(),
    videoPacketsDecoded = videoPackets.get().toDouble(),
    videoFramesDropped = videoDropped.get().toDouble(),
    videoWidth = videoW.get().toDouble(),
    videoHeight = videoH.get().toDouble(),
    currentTimeSeconds = positionMs.get() / 1000.0,
  )

  private fun startPolling(exo: ExoPlayer) {
    detach()
    val runnable = object : Runnable {
      override fun run() {
        positionMs.set(exo.currentPosition)
        exo.videoDecoderCounters?.let {
          videoPackets.set(videoPacketsBase + it.renderedOutputBufferCount)
          videoDropped.set(videoDroppedBase + it.droppedBufferCount)
        }
        exo.audioDecoderCounters?.let {
          audioPackets.set(audioPacketsBase + it.queuedInputBufferCount)
        }
        mainHandler.postDelayed(this, STATS_INTERVAL_MS)
      }
    }
    tick = runnable
    mainHandler.post(runnable)
  }

  private companion object {
    const val STATS_INTERVAL_MS = 750L
  }
}
