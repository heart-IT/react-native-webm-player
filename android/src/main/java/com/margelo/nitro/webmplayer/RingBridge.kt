package com.margelo.nitro.webmplayer

import java.nio.ByteBuffer

/**
 * The JNI surface over the shared C++ WebMStreamBuffer ring.
 *
 * Split from HybridWebmPlayer so the player holds playback policy and this
 * holds the native handle contract: handles come from RingRegistry, are never
 * reused, and every call resolves the handle to a shared_ptr on the C++ side,
 * so a stale handle fails cleanly instead of dangling.
 *
 * The native library is loaded by WebmPlayerPackage before any player exists.
 */
internal object RingBridge {
  external fun create(): Long
  external fun destroy(handle: Long)
  external fun write(handle: Long, directBuffer: ByteBuffer, length: Int): Int
  external fun read(handle: Long, dst: ByteArray, offset: Int, length: Int, timeoutMs: Int): Int
  external fun droppedBytes(handle: Long): Long
  external fun fillRatio(handle: Long): Double
  external fun setEndOfStream(handle: Long)
  external fun clear(handle: Long)
  external fun shutdown(handle: Long)
}
