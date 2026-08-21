package com.margelo.nitro.webmplayer

import androidx.media3.common.MediaItem
import androidx.media3.common.MimeTypes
import androidx.media3.common.util.UnstableApi
import androidx.media3.datasource.DataSource
import androidx.media3.exoplayer.DefaultLoadControl
import androidx.media3.exoplayer.LoadControl
import androidx.media3.exoplayer.source.MediaSource
import androidx.media3.exoplayer.source.ProgressiveMediaSource
import androidx.media3.extractor.DefaultExtractorsFactory
import androidx.media3.extractor.mkv.MatroskaExtractor

/**
 * Assembles the ExoPlayer ingestion pieces: the live-tuned LoadControl and a
 * ProgressiveMediaSource reading muxed WebM from ring-backed DataSources.
 */
@UnstableApi
internal object WebmMediaPipeline {

  fun loadControl(): LoadControl =
    DefaultLoadControl.Builder()
      .setBufferDurationsMs(
        MIN_BUFFER_MS, MAX_BUFFER_MS, BUFFER_FOR_PLAYBACK_MS, BUFFER_AFTER_REBUFFER_MS,
      )
      .setBackBuffer(BACK_BUFFER_MS, true)
      .setPrioritizeTimeOverSizeThresholds(true)
      .build()

  /**
   * Fresh DataSource per load: the loader closes its source when a load ends,
   * and a shared instance, once closed, would answer the next load with
   * end-of-input.
   */
  fun mediaSource(newDataSource: () -> DataSource): MediaSource {
    val extractors = DefaultExtractorsFactory()
      .setMatroskaExtractorFlags(MatroskaExtractor.FLAG_DISABLE_SEEK_FOR_CUES)
    val mediaItem = MediaItem.Builder()
      .setUri(RingDataSource.STREAM_URI)
      .setMimeType(MimeTypes.VIDEO_WEBM)
      .build()
    return ProgressiveMediaSource.Factory(DataSource.Factory { newDataSource() }, extractors)
      .createMediaSource(mediaItem)
  }

  // Live-broadcast latency posture: keep only ~a tenth of a second buffered;
  // the 120 ms ceiling also bounds how far a backgrounded player can drain.
  private const val MIN_BUFFER_MS = 50
  private const val MAX_BUFFER_MS = 120
  private const val BUFFER_FOR_PLAYBACK_MS = 30
  private const val BUFFER_AFTER_REBUFFER_MS = 50
  private const val BACK_BUFFER_MS = 50
}
