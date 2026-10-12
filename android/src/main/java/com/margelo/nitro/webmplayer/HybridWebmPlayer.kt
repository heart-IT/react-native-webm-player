package com.margelo.nitro.webmplayer

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.media3.common.AudioAttributes
import androidx.media3.common.C
import androidx.media3.common.util.UnstableApi
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.ui.PlayerView
import com.margelo.nitro.NitroModules
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Android playback: a feeder worklet per broadcast key, piped into ExoPlayer.
 *
 * ExoPlayer is confined to the main thread. Nitro calls arrive on the JS thread,
 * so they flip atomics and post the work to main, and every value the spec can
 * read synchronously is mirrored into an atomic.
 *
 * A lost stream is never resumed: the session is rejoined with a fresh worklet
 * and a fresh media source on the same player.
 */
@UnstableApi
class HybridWebmPlayer : HybridWebmPlayerSpec() {

  private val mainHandler = Handler(Looper.getMainLooper())
  private val stats = PlaybackStats(mainHandler)

  // False once stop() is called, before its main-thread teardown runs, so
  // events still queued for the old session are ignored.
  private val active = AtomicBoolean(false)
  @Volatile private var currentMuted = false
  @Volatile private var metricsSource: WorkletSource? = null
  @Volatile private var healthCallback: ((WebmHealthEvent) -> Unit)? = null
  @Volatile private var lastHealth: WebmHealthEvent? = null

  // Main-thread only, like everything else that touches ExoPlayer.
  private var player: ExoPlayer? = null
  private var surface: PlayerView? = null
  private var context: Context? = null
  private var key: String? = null
  private var source: WorkletSource? = null
  private var broadcastEnded = false
  private var focusLost = false
  private var rejoinAttempts = 0
  private var pendingRejoin: Runnable? = null

  // MARK: - Session

  override fun play(key: String) {
    if (active.get()) stop()
    stats.failed.set(false)
    lastHealth = null
    active.set(true)
    mainHandler.post { startOnMain(key) }
  }

  override fun stop() {
    active.set(false)
    mainHandler.post { teardownOnMain() }
  }

  override fun dispose() {
    stop()
    super.dispose()
  }

  override val playbackState: WebmPlaybackState
    get() = when {
      stats.failed.get() -> WebmPlaybackState.FAILED
      !active.get() -> WebmPlaybackState.IDLE
      stats.rendered.get() -> WebmPlaybackState.PLAYING
      else -> WebmPlaybackState.BUFFERING
    }

  private fun startOnMain(key: String) {
    // A stop() issued before this post ran has already cancelled the session.
    if (!active.get()) return
    val ctx = NitroModules.applicationContext ?: run {
      fail("no application context")
      return
    }
    var exo: ExoPlayer? = null
    try {
      exo = ExoPlayer.Builder(ctx)
        .setLoadControl(WebmMediaPipeline.loadControl())
        .build()
        .apply {
          // handleAudioFocus = true: ExoPlayer owns focus, and its losses and
          // regains reach onAudioFocus through playWhenReady.
          setAudioAttributes(
            AudioAttributes.Builder()
              .setUsage(C.USAGE_MEDIA)
              .setContentType(C.AUDIO_CONTENT_TYPE_MOVIE)
              .build(),
            true,
          )
          volume = if (currentMuted) MUTED_VOLUME else FULL_VOLUME
        }
      stats.attach(exo, playerEvents)
      player = exo
      surface?.player = exo
      context = ctx
      this.key = key
      rejoinAttempts = 0
      focusLost = false
      fireHealth(WebmHealthStatus.BUFFERING, "connecting")
      connect(exo)
      exo.playWhenReady = true
    } catch (e: Exception) {
      Log.e(TAG, "play failed", e)
      // Not yet assigned to `player` if build or attach threw, so teardown
      // cannot reach it, and a built instance carries a live playback thread.
      if (player !== exo) exo?.release()
      fail(e.message ?: e.javaClass.simpleName)
    }
  }

  /** Starts a worklet for the current key and prepares the player on it. */
  private fun connect(exo: ExoPlayer) {
    val src = WorkletSource.start(requireNotNull(context), requireNotNull(key), sourceEvents)
    source = src
    metricsSource = src
    broadcastEnded = false
    stats.newSession()
    exo.setMediaSource(WebmMediaPipeline.mediaSource { StreamDataSource(src.inputStream) })
    exo.prepare()
  }

  private fun rejoin(reason: String, immediate: Boolean = false) {
    if (!active.get() || pendingRejoin != null) return
    stopSource()
    if (!immediate && rejoinAttempts >= REJOIN_BACKOFF_MS.size) {
      fail("rejoin failed: $reason")
      return
    }
    val delayMs = if (immediate) 0L else REJOIN_BACKOFF_MS[rejoinAttempts++]
    fireHealth(WebmHealthStatus.BUFFERING, "rejoining: $reason")
    val task = Runnable {
      pendingRejoin = null
      // stop() may land between the schedule and its teardown.
      if (!active.get()) return@Runnable
      val exo = player ?: return@Runnable
      try {
        connect(exo)
      } catch (e: Exception) {
        Log.e(TAG, "rejoin failed", e)
        fail(e.message ?: e.javaClass.simpleName)
      }
    }
    pendingRejoin = task
    mainHandler.postDelayed(task, delayMs)
  }

  private fun stopSource() {
    source?.stop()
    source = null
  }

  private fun fail(detail: String) {
    fireHealth(WebmHealthStatus.FAILED, detail)
    stats.failed.set(true)
    active.set(false)
    teardownOnMain()
  }

  private fun teardownOnMain() {
    pendingRejoin?.let { mainHandler.removeCallbacks(it) }
    pendingRejoin = null
    stats.detach()
    surface?.player = null
    // Released before the source stops, so the player never sees the pipe end.
    player?.release()
    player = null
    stopSource()
  }

  // MARK: - Session events (main thread)

  private val sourceEvents = object : WorkletSource.Listener {
    override fun onFirstData(source: WorkletSource) {
      if (source === this@HybridWebmPlayer.source) rejoinAttempts = 0
    }

    override fun onStatus(source: WorkletSource, status: String) {
      if (source !== this@HybridWebmPlayer.source) return
      when {
        // The worklet's view of the swarm while it waits for a peer: a log line,
        // not a health event.
        status.startsWith(STATUS_NET) -> Log.i(TAG, "worklet $status")
        status == STATUS_JOINING || status.startsWith(STATUS_CONNECTED) ->
          if (!stats.rendered.get()) fireHealth(WebmHealthStatus.BUFFERING, "$status, waiting for data")
        status == STATUS_ENDED -> {
          // ExoPlayer drains what is buffered, then reaches STATE_ENDED.
          broadcastEnded = true
          source.endInput()
        }
        status.startsWith(STATUS_LAG) || status.startsWith(STATUS_STALLED) ||
          status == STATUS_RECONFIGURED || status.startsWith(STATUS_ERROR) ->
          rejoin(status)
        else -> Log.w(TAG, "ignoring unknown worklet status: $status")
      }
    }

    override fun onEndOfStream(source: WorkletSource) {
      if (source === this@HybridWebmPlayer.source && !broadcastEnded) rejoin("worklet exited")
    }
  }

  private val playerEvents = object : PlaybackStats.Events {
    override fun onHealth(status: WebmHealthStatus, detail: String) = fireHealth(status, detail)

    override fun onFirstFrame() {
      rejoinAttempts = 0
    }

    override fun onEnded() {
      when {
        // A finished broadcast leaves the player stopped; play() starts another.
        broadcastEnded -> {
          fireHealth(WebmHealthStatus.ENDED, "broadcast ended")
          active.set(false)
          teardownOnMain()
        }
        // No source: a rejoin is pending or focus is lost, and this is the old
        // pipe draining.
        source != null -> rejoin("worklet exited")
      }
    }

    // A live source that ExoPlayer rejects (a pipe that ended before its header,
    // a decoder failure) is a lost stream, not a dead player: re-preparing on a
    // fresh worklet recovers it, and the dataless-session limit bounds retries.
    override fun onPlayerError(detail: String) = rejoin("player error: $detail")

    override fun onAudioFocus(held: Boolean) {
      if (!active.get() || broadcastEnded) return
      if (!held) {
        pendingRejoin?.let { mainHandler.removeCallbacks(it) }
        pendingRejoin = null
        stopSource()
        focusLost = true
        fireHealth(WebmHealthStatus.BUFFERING, "audio focus lost")
      } else if (focusLost) {
        focusLost = false
        rejoin("audio focus regained", immediate = true)
      }
    }
  }

  // MARK: - Controls

  override var muted: Boolean
    get() = currentMuted
    set(value) {
      currentMuted = value
      mainHandler.post { player?.volume = if (value) MUTED_VOLUME else FULL_VOLUME }
    }

  // MARK: - Video surface

  /**
   * Called by HybridWebmPlayerView. Not on the spec: the spec is the parity
   * contract with iOS, and a PlayerView has no meaning there.
   *
   * ExoPlayer is main-thread confined, and the surface may be attached before
   * play() has finished creating the player, so binding is posted and also
   * re-applied when the player appears.
   */
  fun attachSurface(target: PlayerView) {
    mainHandler.post {
      val previous = surface
      // A replaced view must drop its player reference, or it stays registered
      // as a listener on the ExoPlayer it no longer presents.
      if (previous !== target) previous?.player = null
      surface = target
      target.player = player
    }
  }

  fun detachSurface(target: PlayerView) {
    mainHandler.post {
      if (surface !== target) return@post  // a newer view already took over
      target.player = null
      surface = null
    }
  }

  // MARK: - Health and metrics

  override fun setHealthCallback(callback: (event: WebmHealthEvent) -> Unit) {
    healthCallback = callback
  }

  /** Fires only on change, so a stalled stream does not spam the callback. */
  private fun fireHealth(status: WebmHealthStatus, detail: String) {
    // A user stop emits nothing, including events queued before its teardown.
    if (!active.get()) return
    val event = WebmHealthEvent(status, detail)
    if (lastHealth == event) return
    lastHealth = event
    healthCallback?.invoke(event)
  }

  override fun getMetrics(): WebmPlayerMetrics =
    stats.snapshot(bytesReceived = metricsSource?.bytesReceived?.get() ?: 0L)

  companion object {
    private const val TAG = "WebmPlayer"
    private const val MUTED_VOLUME = 0f
    private const val FULL_VOLUME = 1f
    private val REJOIN_BACKOFF_MS = longArrayOf(500, 1_000, 2_000, 4_000, 4_000)
    private const val STATUS_NET = "net "
    private const val STATUS_JOINING = "joining"
    private const val STATUS_CONNECTED = "connected"
    private const val STATUS_ENDED = "ended"
    private const val STATUS_LAG = "lag"
    private const val STATUS_STALLED = "stalled"
    private const val STATUS_RECONFIGURED = "reconfigured"
    private const val STATUS_ERROR = "error"
  }
}
