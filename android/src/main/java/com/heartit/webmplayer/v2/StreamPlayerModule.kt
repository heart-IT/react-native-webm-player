package com.heartit.webmplayer.v2

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.View
import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.MimeTypes
import androidx.media3.common.PlaybackException
import androidx.media3.common.Player
import androidx.media3.common.VideoSize
import androidx.media3.common.util.UnstableApi
import androidx.media3.datasource.DataSource
import androidx.media3.datasource.DataSpec
import androidx.media3.datasource.TransferListener
import androidx.media3.exoplayer.DefaultLoadControl
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.exoplayer.source.MediaSource
import androidx.media3.exoplayer.source.ProgressiveMediaSource
import androidx.media3.extractor.DefaultExtractorsFactory
import androidx.media3.extractor.mkv.MatroskaExtractor
import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod
import com.facebook.react.bridge.WritableMap
import com.facebook.react.modules.core.DeviceEventManagerModule
import java.lang.ref.WeakReference

/**
 * V2 Android playback module. Mirrors the iOS PlaybackEngineV2 surface from JS.
 *
 * Architecture: ExoPlayer + custom DataSource that pulls from a JNI-backed
 * WebMStreamBuffer (cpp/common/WebMStreamBuffer). All A/V sync, codec selection,
 * and rendering is delegated to ExoPlayer/MediaCodec/AudioTrack — we do not
 * decode anything ourselves.
 *
 * Ported from call-doctor-mobile (battle-tested).
 */
@UnstableApi
class StreamPlayerModule(reactContext: ReactApplicationContext) :
    ReactContextBaseJavaModule(reactContext) {

    companion object {
        private const val TAG = "WebmPlayerV2"
        private const val STATS_INTERVAL_MS = 750L
        private const val LIVE_LAG_THRESHOLD_BYTES = 2 * 1024 * 1024
        private val viewRegistry = mutableMapOf<Int, WeakReference<StreamPlayerView>>()

        fun registerView(viewId: Int, view: StreamPlayerView) {
            viewRegistry[viewId] = WeakReference(view)
        }

        fun unregisterView(viewId: Int) { viewRegistry.remove(viewId) }
    }

    private val mainHandler = Handler(Looper.getMainLooper())
    private var player: ExoPlayer? = null
    private var dataSource: WebMRingDataSource? = null
    private val isInitialized = java.util.concurrent.atomic.AtomicBoolean(false)
    private var audioManager: AudioManager? = null
    private var focusRequest: AudioFocusRequest? = null
    private var statsRunnable: Runnable? = null

    // Background/foreground state. Player is paused when the app backgrounds and
    // (optionally) auto-resumed + jumped to live when it foregrounds again.
    private var isInBackground = false
    private var wasPlayingBeforeBackground = false

    override fun getName(): String = "WebmPlayerV2Module"

    // JNI: backed by WebMStreamBuffer singleton in cpp
    private external fun nativeFeedData(data: ByteArray, offset: Int, length: Int): Int
    private external fun nativeReadSharedWebM(dst: ByteArray, offset: Int, length: Int): Int
    private external fun nativeGoToLive()
    private external fun nativeIsBehindLive(thresholdBytes: Int): Boolean
    private external fun nativeClear()
    private external fun nativeSetEndOfStream()

    @ReactMethod
    fun start(promise: Promise) {
        if (isInitialized.get()) { promise.resolve(true); return }
        mainHandler.post {
            try {
                audioManager = reactApplicationContext.getSystemService(Context.AUDIO_SERVICE) as AudioManager
                val loadControl = DefaultLoadControl.Builder()
                    .setBufferDurationsMs(50, 120, 30, 50)
                    .setBackBuffer(50, true)
                    .setPrioritizeTimeOverSizeThresholds(true)
                    .build()

                player = ExoPlayer.Builder(reactApplicationContext)
                    .setLoadControl(loadControl)
                    .build()
                    .apply {
                        setAudioAttributes(
                            androidx.media3.common.AudioAttributes.Builder()
                                .setUsage(C.USAGE_MEDIA)
                                .setContentType(C.AUDIO_CONTENT_TYPE_MOVIE)
                                .build(),
                            false
                        )
                    }

                setupAudioFocus()
                setupListeners()
                startStatsPolling()

                dataSource = WebMRingDataSource(::nativeReadSharedWebM)
                val factory = DataSource.Factory { dataSource!! }
                val extractors = DefaultExtractorsFactory()
                    .setMatroskaExtractorFlags(MatroskaExtractor.FLAG_DISABLE_SEEK_FOR_CUES)
                val mediaItem = MediaItem.Builder()
                    .setUri("custom://webm-stream")
                    .setMimeType(MimeTypes.VIDEO_WEBM)
                    .build()
                val mediaSource: MediaSource = ProgressiveMediaSource.Factory(factory, extractors)
                    .createMediaSource(mediaItem)
                player?.setMediaSource(mediaSource)
                player?.prepare()
                player?.playWhenReady = true

                isInitialized.set(true)
                promise.resolve(true)
            } catch (e: Exception) {
                Log.e(TAG, "start failed", e)
                releaseInternal()
                promise.reject("START_ERROR", e.message, e)
            }
        }
    }

    @ReactMethod
    fun stop(promise: Promise) {
        mainHandler.post {
            try {
                releaseInternal()
                isInitialized.set(false)
                promise.resolve(true)
            } catch (e: Exception) {
                promise.reject("STOP_ERROR", e.message, e)
            }
        }
    }

    @ReactMethod
    fun pause(promise: Promise) {
        mainHandler.post {
            try { player?.pause(); promise.resolve(true) }
            catch (e: Exception) { promise.reject("PAUSE_ERROR", e.message, e) }
        }
    }

    @ReactMethod
    fun resume(promise: Promise) {
        mainHandler.post {
            try { player?.play(); promise.resolve(true) }
            catch (e: Exception) { promise.reject("RESUME_ERROR", e.message, e) }
        }
    }

    @ReactMethod
    fun feedData(base64Data: String, promise: Promise) {
        try {
            // Base64 string from JS is ~4/3 the binary size; decoded once natively.
            // For a 64KB chunk this is one bridge crossing instead of 65,536 (the previous
            // ReadableArray-of-int approach). Total time goes from ~hundreds-of-ms to <1ms.
            val buf = android.util.Base64.decode(base64Data, android.util.Base64.NO_WRAP)
            val wrote = nativeFeedData(buf, 0, buf.size)
            promise.resolve(wrote == buf.size)
        } catch (e: Exception) {
            promise.reject("FEED_ERROR", e.message, e)
        }
    }

    @ReactMethod
    fun setMuted(muted: Boolean, promise: Promise) {
        mainHandler.post {
            player?.volume = if (muted) 0f else 1f
            promise.resolve(true)
        }
    }

    @ReactMethod
    fun setGain(gain: Double, promise: Promise) {
        mainHandler.post {
            player?.volume = gain.toFloat().coerceIn(0f, 2f)
            promise.resolve(true)
        }
    }

    @ReactMethod
    fun setPlaybackRate(rate: Double, promise: Promise) {
        mainHandler.post {
            try {
                player?.setPlaybackSpeed(rate.toFloat().coerceIn(0.5f, 2.0f))
                promise.resolve(true)
            } catch (e: Exception) {
                promise.reject("RATE_ERROR", e.message, e)
            }
        }
    }

    @ReactMethod
    fun appDidEnterBackground(promise: Promise) {
        mainHandler.post {
            try {
                isInBackground = true
                wasPlayingBeforeBackground = player?.isPlaying == true
                if (wasPlayingBeforeBackground) player?.pause()
                promise.resolve(true)
            } catch (e: Exception) {
                promise.reject("BACKGROUND_ERROR", e.message, e)
            }
        }
    }

    @ReactMethod
    fun appDidEnterForeground(promise: Promise) {
        mainHandler.post {
            try {
                isInBackground = false
                if (wasPlayingBeforeBackground) {
                    // The buffer may have stale data after a background pause;
                    // jump to live so playback catches up cleanly.
                    if (nativeIsBehindLive(LIVE_LAG_THRESHOLD_BYTES)) nativeGoToLive()
                    player?.play()
                }
                promise.resolve(true)
            } catch (e: Exception) {
                promise.reject("FOREGROUND_ERROR", e.message, e)
            }
        }
    }

    @ReactMethod
    fun resetStream(promise: Promise) {
        try { nativeClear(); promise.resolve(true) }
        catch (e: Exception) { promise.reject("RESET_ERROR", e.message, e) }
    }

    @ReactMethod
    fun goToLive(promise: Promise) {
        mainHandler.post {
            try {
                if (nativeIsBehindLive(LIVE_LAG_THRESHOLD_BYTES)) {
                    nativeGoToLive()
                    player?.apply {
                        seekTo(C.TIME_UNSET)
                        playWhenReady = true
                    }
                    promise.resolve(true)
                } else {
                    promise.resolve(false)
                }
            } catch (e: Exception) {
                promise.reject("GOLIVE_ERROR", e.message, e)
            }
        }
    }

    @ReactMethod
    fun attachToView(viewId: Int, promise: Promise) {
        mainHandler.post {
            val view = viewRegistry[viewId]?.get()
            val p = player
            if (view == null) { promise.reject("ATTACH_ERROR", "View not found"); return@post }
            if (p == null) { promise.reject("ATTACH_ERROR", "Player not initialized"); return@post }
            view.setPlayer(p)
            promise.resolve(true)
        }
    }

    @ReactMethod
    fun detachFromView(viewId: Int, promise: Promise) {
        mainHandler.post {
            viewRegistry[viewId]?.get()?.setPlayer(null)
            promise.resolve(true)
        }
    }

    private fun setupAudioFocus() {
        val attrs = AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_MEDIA)
            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
            .build()
        val listener = AudioManager.OnAudioFocusChangeListener { focusChange ->
            mainHandler.post { handleFocus(focusChange) }
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            focusRequest = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                .setAudioAttributes(attrs)
                .setOnAudioFocusChangeListener(listener)
                .setWillPauseWhenDucked(true)
                .build()
            audioManager?.requestAudioFocus(focusRequest!!)
        }
    }

    private fun handleFocus(focusChange: Int) {
        when (focusChange) {
            AudioManager.AUDIOFOCUS_LOSS,
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT -> player?.pause()
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK -> player?.volume = 0.2f
            AudioManager.AUDIOFOCUS_GAIN -> player?.volume = 1.0f
        }
    }

    private fun setupListeners() {
        player?.addListener(object : Player.Listener {
            override fun onPlaybackStateChanged(state: Int) {
                emit("onPlaybackStateChanged", Arguments.createMap().apply { putInt("state", state) })
            }
            override fun onPlayerError(error: PlaybackException) {
                Log.e(TAG, "Player error: ${error.message}", error)
                emit("onPlayerError", Arguments.createMap().apply {
                    putString("error", error.message ?: "Unknown")
                    putString("errorCode", error.errorCode.toString())
                })
            }
            override fun onIsPlayingChanged(isPlaying: Boolean) {
                emit("onIsPlayingChanged", Arguments.createMap().apply { putBoolean("isPlaying", isPlaying) })
            }
            override fun onVideoSizeChanged(videoSize: VideoSize) {
                emit("onVideoSizeChanged", Arguments.createMap().apply {
                    putInt("width", videoSize.width); putInt("height", videoSize.height)
                })
            }
            override fun onRenderedFirstFrame() {
                emit("onRenderedFirstFrame", Arguments.createMap())
            }
        })
    }

    private fun startStatsPolling() {
        statsRunnable = object : Runnable {
            override fun run() {
                player?.let { p ->
                    val stats = Arguments.createMap().apply {
                        putDouble("currentTimeSeconds", p.currentPosition / 1000.0)
                        putDouble("bufferedSeconds", p.bufferedPosition / 1000.0)
                        putBoolean("isPlaying", p.isPlaying)
                        putInt("playbackState", p.playbackState)
                    }
                    emit("onStatus", stats)
                }
                mainHandler.postDelayed(this, STATS_INTERVAL_MS)
            }
        }
        mainHandler.post(statsRunnable!!)
    }

    private fun releaseInternal() {
        statsRunnable?.let { mainHandler.removeCallbacks(it) }
        statsRunnable = null
        try { nativeSetEndOfStream() } catch (_: Throwable) {}
        player?.release()
        player = null
        dataSource = null
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            focusRequest?.let { audioManager?.abandonAudioFocusRequest(it) }
            focusRequest = null
        }
    }

    private fun emit(name: String, params: WritableMap) {
        try {
            reactApplicationContext
                .getJSModule(DeviceEventManagerModule.RCTDeviceEventEmitter::class.java)
                .emit(name, params)
        } catch (e: Exception) {
            Log.e(TAG, "Failed to emit $name", e)
        }
    }

    /** ExoPlayer DataSource that pulls from the JNI-backed WebMStreamBuffer. */
    private class WebMRingDataSource(
        private val reader: (ByteArray, Int, Int) -> Int
    ) : DataSource {
        private var opened = false
        override fun open(dataSpec: DataSpec): Long { opened = true; return C.LENGTH_UNSET.toLong() }
        override fun read(buffer: ByteArray, offset: Int, length: Int): Int {
            if (!opened) return C.RESULT_END_OF_INPUT
            val r = reader(buffer, offset, length)
            return when {
                r > 0 -> r
                r == -1 -> C.RESULT_END_OF_INPUT
                else -> 0
            }
        }
        override fun close() { opened = false }
        override fun addTransferListener(transferListener: TransferListener) {}
        override fun getUri(): android.net.Uri = android.net.Uri.parse("custom://webm-stream")
    }
}
