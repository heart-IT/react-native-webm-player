package com.margelo.nitro.webmplayer

import android.net.Uri
import androidx.media3.common.C
import androidx.media3.common.util.UnstableApi
import androidx.media3.datasource.DataSource
import androidx.media3.datasource.DataSpec
import androidx.media3.datasource.TransferListener
import java.io.IOException
import java.io.InputStream

/**
 * Bridges ExoPlayer's loader to the live WebM byte stream. The stream belongs
 * to the WorkletSource that writes it, so [close] leaves it open: the loader
 * closes its DataSource at the end of every load, not at the end of the stream.
 */
@UnstableApi
internal class StreamDataSource(private val stream: InputStream) : DataSource {

  @Throws(IOException::class)
  override fun open(dataSpec: DataSpec): Long {
    // A live stream has no random access. ExoPlayer only passes a position when
    // retrying a failed load from an offset; serving sequential bytes as if
    // they were that offset would silently corrupt the extractor, so fail loud.
    if (dataSpec.position != 0L) {
      throw IOException("stream source is not seekable (requested position ${dataSpec.position})")
    }
    return C.LENGTH_UNSET.toLong()
  }

  @Throws(IOException::class)
  override fun read(buffer: ByteArray, offset: Int, length: Int): Int {
    if (length == 0) return 0
    val n = stream.read(buffer, offset, length)
    return if (n < 0) C.RESULT_END_OF_INPUT else n
  }

  override fun close() = Unit

  override fun addTransferListener(transferListener: TransferListener) = Unit

  override fun getUri(): Uri = Uri.parse(STREAM_URI)

  companion object {
    const val STREAM_URI = "webmplayer://stream"
  }
}
