package com.margelo.nitro.webmplayer

import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.MimeTypes
import androidx.media3.common.util.UnstableApi
import androidx.media3.datasource.DataSource
import androidx.media3.exoplayer.DefaultLoadControl
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.exoplayer.source.MediaSource
import androidx.media3.exoplayer.source.ProgressiveMediaSource
import androidx.media3.extractor.DefaultExtractorsFactory
import androidx.media3.extractor.mkv.MatroskaExtractor
import androidx.media3.ui.PlayerView
import com.margelo.nitro.NitroModules
import com.margelo.nitro.core.ArrayBuffer
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Android playback: ExoPlayer reading from a WebMStreamBuffer-backed DataSource.
 *
 * ExoPlayer is confined to the main thread. Nitro calls arrive on the JS thread,
 * so mutations are posted to main and every value the spec can read is mirrored
 * into an atomic by main-thread listeners. Nothing here touches the player from
 * another thread.
 *
 * `feedData` is the exception and deliberately so: it goes straight to the ring
 * over JNI without a main-thread hop, because it runs at stream bitrate.
 */
@UnstableApi
class HybridWebmPlayer : HybridWebmPlayerSpec() {

  private external fun nativeCreateRing(): Long
  private external fun nativeDestroyRing(handle: Long)
  private external fun nativeWrite(
    handle: Long,
    directBuffer: java.nio.ByteBuffer,
    length: Int,
  ): Int
  private external fun nativeRead(
    handle: Long,
    dst: ByteArray,
    offset: Int,
    length: Int,
    timeoutMs: Int,
  ): Int
  private external fun nativeSetEndOfStream(handle: Long)
  private external fun nativeClear(handle: Long)
  private external fun nativeShutdown(handle: Long)
  private external fun nativeGoToLive(handle: Long)
  private external fun nativeIsBehindLive(handle: Long, thresholdBytes: Int): Boolean

  private val mainHandler = Handler(Looper.getMainLooper())

  @Volatile private var ringHandle: Long = 0L
  @Volatile private var player: ExoPlayer? = null
  private var focus: AudioFocusController? = null
  // Main-thread only, like everything else that touches ExoPlayer.
  private var surface: PlayerView? = null
  private var routes: AudioRouteMonitor? = null
  private var healthCallback: ((WebmHealthEvent) -> Unit)? = null
  @Volatile private var lastHealth: WebmHealthStatus? = null

  private val running = AtomicBoolean(false)
  private val paused = AtomicBoolean(false)
  // Kept apart from `paused`: returning to the foreground must not start playing
  // a stream JS had explicitly paused before the app went away.
  private val backgrounded = AtomicBoolean(false)

  private val stats = PlaybackStats(mainHandler)
  private var lifecycle: HostLifecycleObserver? = null

  @Volatile private var currentGain: Double = 1.0
  @Volatile private var currentMuted: Boolean = false
  @Volatile private var currentRate: Double = 1.0

  // MARK: - Lifecycle

  override fun start(): Boolean {
    if (running.get()) return true
    lastHealth = null
    // Created synchronously so feedData() can buffer while ExoPlayer spins up.
    ringHandle = nativeCreateRing()
    if (ringHandle == 0L) return false
    running.set(true)
    paused.set(false)
    stats.failed.set(false)
    mainHandler.post { startOnMain() }
    return true
  }

  private fun startOnMain() {
    val context = NitroModules.applicationContext ?: run {
      Log.e(TAG, "no application context; cannot create ExoPlayer")
      stats.failed.set(true)
      return
    }
    try {
      val loadControl = DefaultLoadControl.Builder()
        .setBufferDurationsMs(50, 120, 30, 50)
        .setBackBuffer(50, true)
        .setPrioritizeTimeOverSizeThresholds(true)
        .build()

      val exo = ExoPlayer.Builder(context)
        .setLoadControl(loadControl)
        .build()
        .apply {
          setAudioAttributes(
            androidx.media3.common.AudioAttributes.Builder()
              .setUsage(C.USAGE_MEDIA)
              .setContentType(C.AUDIO_CONTENT_TYPE_MOVIE)
              .build(),
            false,
          )
          volume = if (currentMuted) 0f else currentGain.toFloat()
          setPlaybackSpeed(currentRate.toFloat())
        }

      val handle = ringHandle
      val source = RingDataSource { buf, off, len -> nativeRead(handle, buf, off, len, READ_TIMEOUT_MS) }
      val extractors = DefaultExtractorsFactory()
        .setMatroskaExtractorFlags(MatroskaExtractor.FLAG_DISABLE_SEEK_FOR_CUES)
      val mediaItem = MediaItem.Builder()
        .setUri(RingDataSource.STREAM_URI)
        .setMimeType(MimeTypes.VIDEO_WEBM)
        .build()
      val mediaSource: MediaSource =
        ProgressiveMediaSource.Factory(DataSource.Factory { source }, extractors)
          .createMediaSource(mediaItem)

      stats.attach(exo) { status, detail -> fireHealth(status, detail) }
      lifecycle = HostLifecycleObserver(
        context,
        onBackground = { suspendForBackground() },
        onForeground = { resumeFromForeground() },
      )
      focus = AudioFocusController(context, mainHandler).also { controller ->
        controller.acquire { change ->
          when (change) {
            AudioFocusController.Change.PAUSE -> player?.pause()
            AudioFocusController.Change.DUCK -> player?.volume = DUCK_VOLUME
            AudioFocusController.Change.RESTORE ->
              player?.volume = if (currentMuted) 0f else currentGain.toFloat()
          }
        }
      }

      exo.setMediaSource(mediaSource)
      exo.prepare()
      exo.playWhenReady = true
      player = exo
      surface?.player = exo
    } catch (e: Exception) {
      Log.e(TAG, "start failed", e)
      stats.failed.set(true)
      releaseOnMain()
    }
  }

  override fun stop(): Boolean {
    if (!running.get()) return true
    running.set(false)
    paused.set(false)
    backgrounded.set(false)
    val handle = ringHandle
    if (handle != 0L) nativeShutdown(handle)  // unblocks a reader inside DataSource.read
    mainHandler.post {
      releaseOnMain()
      // Freed only after the player (and its loader thread) is gone.
      if (handle != 0L) nativeDestroyRing(handle)
      if (ringHandle == handle) ringHandle = 0L
    }
    return true
  }

  override fun pause(): Boolean {
    paused.set(true)
    mainHandler.post { player?.pause() }
    return true
  }

  override fun resume(): Boolean {
    paused.set(false)
    // Backgrounded playback stays stopped until the app returns, matching iOS.
    if (backgrounded.get()) return true
    mainHandler.post { player?.play() }
    return true
  }

  /**
   * Driven by the host activity, not by JS. ExoPlayer keeps its decoders and
   * buffered data across this, so returning resumes where playback stopped;
   * only the surface it renders into goes away and comes back.
   */
  private fun suspendForBackground() {
    if (!running.get() || backgrounded.getAndSet(true)) return
    mainHandler.post { player?.pause() }
  }

  private fun resumeFromForeground() {
    if (!running.get() || !backgrounded.getAndSet(false)) return
    if (paused.get()) return
    mainHandler.post { player?.play() }
  }

  override val isRunning: Boolean
    get() = running.get()

  override val isPaused: Boolean
    get() = paused.get()

  override val playbackState: WebmPlaybackState
    get() = when {
      stats.failed.get() -> WebmPlaybackState.FAILED
      !running.get() -> WebmPlaybackState.IDLE
      paused.get() -> WebmPlaybackState.PAUSED
      stats.isReady -> WebmPlaybackState.PLAYING
      else -> WebmPlaybackState.BUFFERING
    }

  // MARK: - Stream data

  override fun feedData(data: ArrayBuffer): Boolean {
    val handle = ringHandle
    if (handle == 0L) return false
    val size = data.size
    if (size == 0) return false
    // A JS ArrayBuffer is non-owning: getBuffer(false) wraps its memory in a
    // direct ByteBuffer valid only for this call. The ring copies before we
    // return, so nothing outlives it.
    val buffer = data.getBuffer(false)
    if (!buffer.isDirect) return false
    val wrote = nativeWrite(handle, buffer, size)
    stats.bytesFed.addAndGet(wrote.toLong())
    return wrote == size
  }

  override fun setEndOfStream() {
    val handle = ringHandle
    if (handle != 0L) nativeSetEndOfStream(handle)
  }

  override fun resetStream() {
    val handle = ringHandle
    if (handle != 0L) nativeClear(handle)
  }

  // MARK: - Controls

  override var muted: Boolean
    get() = currentMuted
    set(value) {
      currentMuted = value
      mainHandler.post { player?.volume = if (value) 0f else currentGain.toFloat() }
    }

  override var gain: Double
    get() = currentGain
    set(value) {
      val clamped = value.coerceIn(0.0, 2.0)
      currentGain = clamped
      mainHandler.post { if (!currentMuted) player?.volume = clamped.toFloat() }
    }

  override var playbackRate: Double
    get() = currentRate
    set(value) {
      val clamped = value.coerceIn(0.5, 2.0)
      currentRate = clamped
      mainHandler.post { player?.setPlaybackSpeed(clamped.toFloat()) }
    }

  // MARK: - Video surface

  /**
   * Called by HybridWebmPlayerView. Not on the spec: the spec is the parity
   * contract with iOS, and a PlayerView has no meaning there.
   *
   * ExoPlayer is main-thread confined, and the surface may be attached before
   * start() has finished creating the player, so binding is posted and also
   * re-applied when the player appears.
   */
  fun attachSurface(target: PlayerView) {
    mainHandler.post {
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

  // MARK: - Health and routing

  override fun setHealthCallback(callback: (event: WebmHealthEvent) -> Unit) {
    healthCallback = callback
  }

  /** Fires only on transition, so a stalled stream does not spam the callback. */
  private fun fireHealth(status: WebmHealthStatus, detail: String) {
    if (lastHealth == status) return
    lastHealth = status
    healthCallback?.invoke(WebmHealthEvent(status, detail))
  }

  override val currentAudioRoute: WebmAudioRoute
    get() = routeMonitor().currentRoute()

  override fun getAvailableAudioRoutes(): Array<WebmAudioRoute> =
    routeMonitor().availableRoutes().toTypedArray()

  override fun setRouteChangeCallback(callback: (route: WebmAudioRoute) -> Unit) {
    routeMonitor().setCallback(callback)
  }

  /** Created lazily: routing is queryable before start() and after stop(). */
  private fun routeMonitor(): AudioRouteMonitor {
    routes?.let { return it }
    val context = requireNotNull(NitroModules.applicationContext) {
      "no application context; cannot query audio routes"
    }
    return AudioRouteMonitor(context, mainHandler).also { routes = it }
  }

  // MARK: - Metrics

  override fun getMetrics(): WebmPlayerMetrics =
    stats.snapshot(currentRate, currentMuted, currentGain)

  // MARK: - Main-thread wiring

  private fun releaseOnMain() {
    lifecycle?.invalidate()
    lifecycle = null
    stats.detach()
    surface?.player = null
    player?.release()
    player = null
    focus?.release()
    focus = null
  }

  companion object {
    private const val TAG = "WebmPlayer"
    private const val STATS_INTERVAL_MS = 750L
    private const val READ_TIMEOUT_MS = 50
    private const val DUCK_VOLUME = 0.2f
  }
}
