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
 * Counters are cumulative across start/stop cycles, matching iOS.
 */
@UnstableApi
class PlaybackStats(private val mainHandler: Handler) {

  val bytesFed = AtomicLong(0)
  val failed = AtomicBoolean(false)

  private val audioPackets = AtomicLong(0)
  private val videoPackets = AtomicLong(0)
  private val audioUnderruns = AtomicLong(0)
  private val videoDropped = AtomicLong(0)
  private val videoW = AtomicInteger(0)
  private val videoH = AtomicInteger(0)
  private val positionMs = AtomicLong(0)
  private val exoState = AtomicInteger(Player.STATE_IDLE)

  private var tick: Runnable? = null

  // Decoder counters restart at zero with every ExoPlayer instance (and every
  // re-prepare after a stream reset); these bases carry the totals accumulated
  // so far, so the atomics stay cumulative. Main thread only, like the poll.
  private var audioPacketsBase = 0L
  private var videoPacketsBase = 0L
  private var videoDroppedBase = 0L

  val isReady: Boolean
    get() = exoState.get() == Player.STATE_READY

  /** Wires listeners and starts the counter poll. Main thread only. */
  fun attach(exo: ExoPlayer, onHealth: (WebmHealthStatus, String) -> Unit) {
    rebase()
    // A stale READY from the previous cycle would read as PLAYING before this
    // player's first onPlaybackStateChanged.
    exoState.set(Player.STATE_IDLE)
    exo.addListener(object : Player.Listener {
      override fun onPlaybackStateChanged(state: Int) {
        exoState.set(state)
        when (state) {
          Player.STATE_READY -> onHealth(WebmHealthStatus.PLAYING, "ready")
          Player.STATE_BUFFERING -> onHealth(WebmHealthStatus.BUFFERING, "buffering")
          Player.STATE_ENDED -> onHealth(WebmHealthStatus.ENDED, "end of stream")
          else -> Unit
        }
      }

      override fun onPlayerError(error: PlaybackException) {
        failed.set(true)
        onHealth(WebmHealthStatus.FAILED, error.errorCodeName)
      }

      override fun onVideoSizeChanged(videoSize: VideoSize) {
        videoW.set(videoSize.width)
        videoH.set(videoSize.height)
      }
    })

    exo.addAnalyticsListener(object : AnalyticsListener {
      override fun onAudioUnderrun(
        eventTime: AnalyticsListener.EventTime,
        bufferSize: Int,
        bufferSizeMs: Long,
        elapsedSinceLastFeedMs: Long,
      ) {
        audioUnderruns.incrementAndGet()
      }
    })

    startPolling(exo)
  }

  /** Stops the poll. Main thread only. Safe to call repeatedly. */
  fun detach() {
    tick?.let { mainHandler.removeCallbacks(it) }
    tick = null
  }

  /** Re-captures the cumulative bases. Call whenever decoders restart. */
  fun rebase() {
    audioPacketsBase = audioPackets.get()
    videoPacketsBase = videoPackets.get()
    videoDroppedBase = videoDropped.get()
  }

  fun snapshot(rate: Double, muted: Boolean, gain: Double) = WebmPlayerMetrics(
    bytesFedTotal = bytesFed.get().toDouble(),
    audioPacketsDecoded = audioPackets.get().toDouble(),
    videoPacketsDecoded = videoPackets.get().toDouble(),
    audioUnderruns = audioUnderruns.get().toDouble(),
    videoFramesDropped = videoDropped.get().toDouble(),
    // ExoPlayer conceals internally and does not report recovered frames, so
    // this stays 0 on Android rather than inventing a number.
    audioFramesRecovered = 0.0,
    videoWidth = videoW.get().toDouble(),
    videoHeight = videoH.get().toDouble(),
    currentTimeSeconds = positionMs.get() / 1000.0,
    playbackRate = rate,
    muted = muted,
    gain = gain,
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
