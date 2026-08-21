package com.margelo.nitro.webmplayer

import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.media3.common.C
import androidx.media3.common.util.UnstableApi
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.exoplayer.source.MediaSource
import androidx.media3.ui.PlayerView
import com.margelo.nitro.NitroModules
import com.margelo.nitro.core.ArrayBuffer
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong

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

  private val mainHandler = Handler(Looper.getMainLooper())

  private val ringHandle = AtomicLong(0L)
  @Volatile private var player: ExoPlayer? = null
  // Main-thread only, like everything else that touches ExoPlayer.
  private var surface: PlayerView? = null
  private var routes: AudioRouteMonitor? = null
  @Volatile private var healthCallback: ((WebmHealthEvent) -> Unit)? = null
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
    val handle = nativeCreateRing()
    if (handle == 0L) return false
    ringHandle.set(handle)
    running.set(true)
    paused.set(false)
    stats.failed.set(false)
    mainHandler.post { startOnMain() }
    return true
  }

  private fun startOnMain() {
    val handle = ringHandle.get()
    val context = NitroModules.applicationContext ?: run {
      Log.e(TAG, "no application context; cannot create ExoPlayer")
      failOnMain(handle, "no application context")
      return
    }
    try {
      val exo = ExoPlayer.Builder(context)
        .setLoadControl(WebmMediaPipeline.loadControl())
        .build()
        .apply {
          // handleAudioFocus = true: ExoPlayer owns focus, so losses, ducks and
          // regains flow through playWhenReady/volume and the mirrored state
          // stays truthful.
          setAudioAttributes(
            androidx.media3.common.AudioAttributes.Builder()
              .setUsage(C.USAGE_MEDIA)
              .setContentType(C.AUDIO_CONTENT_TYPE_MOVIE)
              .build(),
            true,
          )
          volume = if (currentMuted) 0f else currentGain.toFloat()
          setPlaybackSpeed(currentRate.toFloat())
        }

      stats.attach(exo) { status, detail -> fireHealth(status, detail) }
      lifecycle = HostLifecycleObserver(
        context,
        onBackground = { suspendForBackground() },
        onForeground = { resumeFromForeground() },
      )

      exo.setMediaSource(buildMediaSource(handle))
      exo.prepare()
      exo.playWhenReady = true
      player = exo
      surface?.player = exo
    } catch (e: Exception) {
      Log.e(TAG, "start failed", e)
      failOnMain(handle, e.message ?: e.javaClass.simpleName)
    }
  }

  /**
   * A player that failed to come up must not report running, and nothing will
   * ever read this cycle's ring, so it is freed here — stop() early-returns
   * once running is false. The CAS leaves a ring a newer start() created alone.
   */
  private fun failOnMain(handle: Long, detail: String) {
    stats.failed.set(true)
    running.set(false)
    fireHealth(WebmHealthStatus.FAILED, detail)
    releaseOnMain()
    if (handle != 0L && ringHandle.compareAndSet(handle, 0L)) {
      nativeShutdown(handle)
      nativeDestroyRing(handle)
    }
  }

  private fun buildMediaSource(handle: Long): MediaSource =
    WebmMediaPipeline.mediaSource {
      RingDataSource { buf, off, len -> nativeRead(handle, buf, off, len, READ_TIMEOUT_MS) }
    }

  override fun stop(): Boolean {
    if (!running.get()) return true
    running.set(false)
    paused.set(false)
    backgrounded.set(false)
    val handle = ringHandle.get()
    if (handle != 0L) nativeShutdown(handle)  // unblocks a reader inside DataSource.read
    mainHandler.post {
      releaseOnMain()
      // Freed only after the player (and its loader thread) is gone. The CAS
      // leaves a ring a newer start() created alone.
      if (handle != 0L) nativeDestroyRing(handle)
      ringHandle.compareAndSet(handle, 0L)
    }
    return true
  }

  override fun dispose() {
    stop()
    routes?.release()
    routes = null
    super.dispose()
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
    val handle = ringHandle.get()
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
    val handle = ringHandle.get()
    if (handle != 0L) nativeSetEndOfStream(handle)
  }

  override fun resetStream() {
    val handle = ringHandle.get()
    if (handle == 0L) return
    nativeClear(handle)
    mainHandler.post {
      val exo = player ?: return@post
      // A new stream begins with its own EBML header, and the running
      // MatroskaExtractor is mid-cluster — rebuild the pipeline rather than
      // feed a header into it. Mirrors the iOS pump's demuxer.reset().
      exo.stop()
      exo.setMediaSource(buildMediaSource(handle))
      exo.prepare()
      stats.rebase()
      exo.playWhenReady = !paused.get() && !backgrounded.get()
      fireHealth(WebmHealthStatus.BUFFERING, "stream reset")
    }
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
  }

  companion object {
    private const val TAG = "WebmPlayer"
    private const val READ_TIMEOUT_MS = 50
  }
}
