package com.margelo.nitro.webmplayer

import android.net.Uri
import androidx.media3.common.C
import androidx.media3.common.util.UnstableApi
import androidx.media3.datasource.DataSource
import androidx.media3.datasource.DataSpec
import androidx.media3.datasource.TransferListener
import java.io.IOException
import java.io.InterruptedIOException

/**
 * Bridges ExoPlayer's loader to the WebMStreamBuffer ring.
 *
 * `DataReader.read` is documented to block until at least one byte is available
 * or the input has ended; returning 0 for a non-zero length breaks that contract
 * and spins the loader. The ring's timed read returns 0 when it simply has
 * nothing yet, so that case is retried here rather than propagated.
 */
@UnstableApi
internal class RingDataSource(
  private val reader: (ByteArray, Int, Int) -> Int,
) : DataSource {
  @Volatile private var opened = false

  override fun open(dataSpec: DataSpec): Long {
    opened = true
    return C.LENGTH_UNSET.toLong()
  }

  @Throws(IOException::class)
  override fun read(buffer: ByteArray, offset: Int, length: Int): Int {
    if (length == 0) return 0
    while (true) {
      if (!opened) return C.RESULT_END_OF_INPUT
      if (Thread.currentThread().isInterrupted) throw InterruptedIOException()
      val n = reader(buffer, offset, length)
      when {
        n > 0 -> return n
        // Ring shut down, or drained after end of stream.
        n < 0 -> return C.RESULT_END_OF_INPUT
        // n == 0: timed out with the stream still live — keep blocking.
      }
    }
  }

  override fun close() {
    opened = false
  }

  override fun addTransferListener(transferListener: TransferListener) = Unit

  override fun getUri(): Uri = Uri.parse(STREAM_URI)

  companion object {
    const val STREAM_URI = "webmplayer://stream"
  }
}
