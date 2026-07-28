package com.margelo.nitro.webmplayer

import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.MimeTypes
import androidx.media3.common.PlaybackException
import androidx.media3.common.Player
import androidx.media3.common.VideoSize
import androidx.media3.common.util.UnstableApi
import androidx.media3.datasource.DataSource
import androidx.media3.exoplayer.DefaultLoadControl
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.exoplayer.analytics.AnalyticsListener
import androidx.media3.exoplayer.source.MediaSource
import androidx.media3.exoplayer.source.ProgressiveMediaSource
import androidx.media3.extractor.DefaultExtractorsFactory
import androidx.media3.extractor.mkv.MatroskaExtractor
import com.margelo.nitro.NitroModules
import com.margelo.nitro.core.ArrayBuffer
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
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
  private external fun nativeGoToLive(handle: Long)
  private external fun nativeIsBehindLive(handle: Long, thresholdBytes: Int): Boolean

  private val mainHandler = Handler(Looper.getMainLooper())

  @Volatile private var ringHandle: Long = 0L
  @Volatile private var player: ExoPlayer? = null
  private var focusRequest: AudioFocusRequest? = null
  private var audioManager: AudioManager? = null
  private var statsRunnable: Runnable? = null

  private val running = AtomicBoolean(false)
  private val paused = AtomicBoolean(false)

  // Mirrors of main-thread-only player state, so the spec's synchronous getters
  // never reach across a thread boundary into ExoPlayer.
  private val bytesFed = AtomicLong(0)
  private val audioPackets = AtomicLong(0)
  private val videoPackets = AtomicLong(0)
  private val audioUnderruns = AtomicLong(0)
  private val videoDropped = AtomicLong(0)
  private val videoW = AtomicInteger(0)
  private val videoH = AtomicInteger(0)
  private val positionMs = AtomicLong(0)
  private val exoState = AtomicInteger(Player.STATE_IDLE)
  private val failed = AtomicBoolean(false)

  @Volatile private var currentGain: Double = 1.0
  @Volatile private var currentMuted: Boolean = false
  @Volatile private var currentRate: Double = 1.0

  // MARK: - Lifecycle

  override fun start(): Boolean {
    if (running.get()) return true
    // Created synchronously so feedData() can buffer while ExoPlayer spins up.
    ringHandle = nativeCreateRing()
    if (ringHandle == 0L) return false
    running.set(true)
    paused.set(false)
    failed.set(false)
    mainHandler.post { startOnMain() }
    return true
  }

  private fun startOnMain() {
    val context = NitroModules.applicationContext ?: run {
      Log.e(TAG, "no application context; cannot create ExoPlayer")
      failed.set(true)
      return
    }
    try {
      audioManager = context.getSystemService(android.content.Context.AUDIO_SERVICE) as AudioManager

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

      attachListeners(exo)
      requestAudioFocus()

      exo.setMediaSource(mediaSource)
      exo.prepare()
      exo.playWhenReady = true
      player = exo
      startStatsPolling()
    } catch (e: Exception) {
      Log.e(TAG, "start failed", e)
      failed.set(true)
      releaseOnMain()
    }
  }

  override fun stop(): Boolean {
    if (!running.get()) return true
    running.set(false)
    paused.set(false)
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
    mainHandler.post { player?.play() }
    return true
  }

  override val isRunning: Boolean
    get() = running.get()

  override val isPaused: Boolean
    get() = paused.get()

  override val playbackState: WebmPlaybackState
    get() = when {
      failed.get() -> WebmPlaybackState.FAILED
      !running.get() -> WebmPlaybackState.IDLE
      paused.get() -> WebmPlaybackState.PAUSED
      exoState.get() == Player.STATE_READY -> WebmPlaybackState.PLAYING
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
    bytesFed.addAndGet(wrote.toLong())
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

  // MARK: - Metrics

  override fun getMetrics(): WebmPlayerMetrics = WebmPlayerMetrics(
    bytesFedTotal = bytesFed.get().toDouble(),
    audioPacketsDecoded = audioPackets.get().toDouble(),
    videoPacketsDecoded = videoPackets.get().toDouble(),
    audioUnderruns = audioUnderruns.get().toDouble(),
    videoFramesDropped = videoDropped.get().toDouble(),
    videoWidth = videoW.get().toDouble(),
    videoHeight = videoH.get().toDouble(),
    currentTimeSeconds = positionMs.get() / 1000.0,
    playbackRate = currentRate,
    muted = currentMuted,
    gain = currentGain,
  )

  // MARK: - Main-thread wiring

  private fun attachListeners(exo: ExoPlayer) {
    exo.addListener(object : Player.Listener {
      override fun onPlaybackStateChanged(state: Int) {
        exoState.set(state)
      }

      override fun onPlayerError(error: PlaybackException) {
        Log.e(TAG, "player error: ${error.errorCodeName}", error)
        failed.set(true)
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
  }

  private fun startStatsPolling() {
    val tick = object : Runnable {
      override fun run() {
        player?.let { p ->
          positionMs.set(p.currentPosition)
          p.videoDecoderCounters?.let {
            videoPackets.set(it.renderedOutputBufferCount.toLong())
            videoDropped.set(it.droppedBufferCount.toLong())
          }
          p.audioDecoderCounters?.let {
            audioPackets.set(it.queuedInputBufferCount.toLong())
          }
        }
        mainHandler.postDelayed(this, STATS_INTERVAL_MS)
      }
    }
    statsRunnable = tick
    mainHandler.post(tick)
  }

  private fun requestAudioFocus() {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
    val attrs = AudioAttributes.Builder()
      .setUsage(AudioAttributes.USAGE_MEDIA)
      .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
      .build()
    val request = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
      .setAudioAttributes(attrs)
      .setWillPauseWhenDucked(true)
      .setOnAudioFocusChangeListener({ change -> mainHandler.post { handleFocus(change) } }, mainHandler)
      .build()
    focusRequest = request
    audioManager?.requestAudioFocus(request)
  }

  private fun handleFocus(change: Int) {
    when (change) {
      AudioManager.AUDIOFOCUS_LOSS,
      AudioManager.AUDIOFOCUS_LOSS_TRANSIENT -> player?.pause()
      AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK -> player?.volume = DUCK_VOLUME
      AudioManager.AUDIOFOCUS_GAIN ->
        player?.volume = if (currentMuted) 0f else currentGain.toFloat()
    }
  }

  private fun releaseOnMain() {
    statsRunnable?.let { mainHandler.removeCallbacks(it) }
    statsRunnable = null
    player?.release()
    player = null
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
      focusRequest?.let { audioManager?.abandonAudioFocusRequest(it) }
    }
    focusRequest = null
    audioManager = null
  }

  companion object {
    private const val TAG = "WebmPlayer"
    private const val STATS_INTERVAL_MS = 750L
    private const val READ_TIMEOUT_MS = 50
    private const val DUCK_VOLUME = 0.2f
  }
}
