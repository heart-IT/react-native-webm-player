package com.margelo.nitro.webmplayer

import android.util.Log
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.net.ProtocolException
import java.nio.ByteBuffer

/**
 * De-frames the feeder worklet's IPC stream: framed-stream's 4-byte
 * little-endian length prefix, then a payload whose first byte is the type.
 *
 * IPC reads split and coalesce frames arbitrarily, so this is a state machine
 * fed whatever arrived. Data payloads are passed through in chunks rather than
 * accumulated, so a large cluster never needs a frame-sized allocation.
 */
internal class FrameDecoder(private val sink: Sink) {

  interface Sink {
    @Throws(IOException::class)
    fun onData(bytes: ByteArray, offset: Int, length: Int)
    fun onStatus(text: String)
  }

  private val header = ByteArray(HEADER_SIZE)
  private var headerFill = 0
  private var frameRemaining = 0
  private var frameType = TYPE_PENDING
  private val status = ByteArrayOutputStream()
  private val chunk = ByteArray(CHUNK_SIZE)
  private var warnedUnknownType = false

  @Throws(IOException::class)
  fun feed(buf: ByteBuffer) {
    while (buf.hasRemaining()) {
      if (headerFill < HEADER_SIZE) {
        header[headerFill++] = buf.get()
        if (headerFill == HEADER_SIZE) beginFrame()
        continue
      }
      if (frameType == TYPE_PENDING) {
        frameType = buf.get().toInt() and 0xFF
        frameRemaining--
        status.reset()
      } else {
        val n = minOf(buf.remaining(), frameRemaining, chunk.size)
        buf.get(chunk, 0, n)
        frameRemaining -= n
        when (frameType) {
          TYPE_DATA -> sink.onData(chunk, 0, n)
          TYPE_STATUS -> status.write(chunk, 0, n)
        }
      }
      if (frameRemaining == 0) endFrame()
    }
  }

  @Throws(ProtocolException::class)
  private fun beginFrame() {
    val length = (header[0].toLong() and 0xFF) or
      ((header[1].toLong() and 0xFF) shl 8) or
      ((header[2].toLong() and 0xFF) shl 16) or
      ((header[3].toLong() and 0xFF) shl 24)
    if (length == 0L || length > MAX_FRAME_BYTES) {
      throw ProtocolException("frame length $length")
    }
    frameRemaining = length.toInt()
    frameType = TYPE_PENDING
  }

  private fun endFrame() {
    when (frameType) {
      TYPE_DATA -> Unit
      TYPE_STATUS -> sink.onStatus(status.toString(Charsets.UTF_8.name()))
      else -> if (!warnedUnknownType) {
        warnedUnknownType = true
        Log.w(TAG, "ignoring frame of unknown type $frameType")
      }
    }
    headerFill = 0
  }

  private companion object {
    const val TAG = "WebmPlayer"
    const val HEADER_SIZE = 4
    const val TYPE_PENDING = -1
    const val TYPE_DATA = 0x00
    const val TYPE_STATUS = 0x01
    const val MAX_FRAME_BYTES = 16L * 1024 * 1024
    const val CHUNK_SIZE = 64 * 1024
  }
}
