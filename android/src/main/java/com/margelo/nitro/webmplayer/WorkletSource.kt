package com.margelo.nitro.webmplayer

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.io.PipedInputStream
import java.io.PipedOutputStream
import java.net.ProtocolException
import java.util.UUID
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong
import to.holepunch.bare.kit.IPC
import to.holepunch.bare.kit.Worklet

/**
 * One feeder-worklet session: starts the Bare worklet for a broadcast key and
 * pipes the WebM bytes it sends over IPC into [inputStream].
 *
 * The reader thread owns the worklet and IPC handles from start to teardown, so
 * no bare-kit call races another thread. The blocking pipe write is the
 * backpressure: while ExoPlayer is not reading, this thread stops reading IPC
 * and the worklet's own IPC writes stall.
 *
 * Listener callbacks are posted to the main looper.
 */
internal class WorkletSource private constructor(
  private val worklet: Worklet,
  private val ipc: IPC,
  private val storageDir: File,
  private val listener: Listener,
) {

  interface Listener {
    fun onFirstData(source: WorkletSource)
    fun onStatus(source: WorkletSource, status: String)
    fun onEndOfStream(source: WorkletSource)
  }

  private val mainHandler = Handler(Looper.getMainLooper())
  private val pipeIn = PipedInputStream(PIPE_BYTES)
  private val pipeOut = PipedOutputStream(pipeIn)
  private val stopped = AtomicBoolean(false)
  private val sawData = AtomicBoolean(false)
  private val inputEnded = AtomicBoolean(false)
  // Capacity 1: the readable callback can fire repeatedly while armed, and a
  // single pending wake-up is all the reader needs.
  private val wake = ArrayBlockingQueue<Unit>(1)
  private val reader = Thread(::readLoop, "webmplayer.worklet")

  val bytesReceived = AtomicLong(0)

  val inputStream: InputStream
    get() = pipeIn

  /**
   * Closes the write end so the data source drains what is buffered and then
   * sees end of input. Any frame the worklet sends afterwards ends the reader.
   */
  fun endInput() {
    inputEnded.set(true)
    try {
      pipeOut.close()
    } catch (e: IOException) {
      Log.w(TAG, "closing the pipe failed", e)
    }
  }

  /**
   * Idempotent; any thread. The read end stays open: an ExoPlayer read already
   * in flight then ends with end-of-input rather than an IOException, which
   * the player would surface as a fatal error.
   */
  fun stop() {
    if (!stopped.compareAndSet(false, true)) return
    endInput()
    reader.interrupt()
    reader.join(JOIN_TIMEOUT_MS)
    if (reader.isAlive) Log.w(TAG, "worklet reader still tearing down after ${JOIN_TIMEOUT_MS} ms")
  }

  private fun readLoop() {
    val decoder = FrameDecoder(object : FrameDecoder.Sink {
      override fun onData(bytes: ByteArray, offset: Int, length: Int) {
        pipeOut.write(bytes, offset, length)
        bytesReceived.addAndGet(length.toLong())
        if (sawData.compareAndSet(false, true)) post { listener.onFirstData(this@WorkletSource) }
      }

      override fun onStatus(text: String) = post { listener.onStatus(this@WorkletSource, text) }
    })
    try {
      while (!stopped.get()) {
        val buf = ipc.read()
        if (buf == null) {
          ipc.readable { wake.offer(Unit) }
          // Bounded so a missed wake-up or stop() never strands this thread.
          wake.poll(READ_WAIT_MS, TimeUnit.MILLISECONDS)
          ipc.readable(null)
          continue
        }
        if (buf.limit() == 0) {
          endInput()
          post { listener.onEndOfStream(this) }
          break
        }
        decoder.feed(buf)
      }
    } catch (e: ProtocolException) {
      Log.e(TAG, "feeder protocol error", e)
      post { listener.onStatus(this, "error protocol: ${e.message}") }
    } catch (e: IOException) {
      // Expected when endInput() or stop() closed the pipe under the write.
      // Otherwise the read side died, and the session must not stall silently.
      if (inputEnded.get()) {
        Log.d(TAG, "worklet reader stopped: ${e.message}")
      } else {
        Log.e(TAG, "pipe write failed", e)
        post { listener.onStatus(this, "error pipe: ${e.message}") }
      }
    } catch (_: InterruptedException) {
      Log.d(TAG, "worklet reader interrupted by stop()")
    } finally {
      endInput()
      worklet.terminate()
      ipc.close()
      if (!storageDir.deleteRecursively()) Log.w(TAG, "could not delete $storageDir")
    }
  }

  private fun post(action: () -> Unit) {
    if (stopped.get()) return
    mainHandler.post(action)
  }

  companion object {
    private const val TAG = "WebmPlayer"
    private const val BUNDLE_ASSET = "feeder.bundle"
    // bare-kit requires the filename to end in .bundle to load it as one.
    private const val BUNDLE_FILENAME = "/feeder.bundle"
    private const val STORAGE_ROOT = "webm-player"
    private const val PIPE_BYTES = 4 * 1024 * 1024
    private const val READ_WAIT_MS = 100L
    private const val JOIN_TIMEOUT_MS = 500L

    /** Starts the worklet for [key]. Throws if the bundle or worklet fails to load. */
    @Throws(IOException::class)
    fun start(context: Context, key: String, listener: Listener): WorkletSource {
      // Sessions delete their own directory on teardown. Leftovers from a
      // process that died mid-session sit in the cache directory, which the
      // system reclaims; sweeping them here would also hit another live player.
      val storageDir = File(File(context.cacheDir, STORAGE_ROOT), UUID.randomUUID().toString())
      if (!storageDir.mkdirs()) throw IOException("could not create $storageDir")

      val worklet = Worklet(Worklet.Options())
      try {
        context.assets.open(BUNDLE_ASSET).use { bundle ->
          worklet.start(BUNDLE_FILENAME, bundle, arrayOf(key, storageDir.absolutePath))
        }
      } catch (e: IOException) {
        worklet.terminate()
        storageDir.deleteRecursively()
        throw e
      }
      val source = WorkletSource(worklet, IPC(worklet), storageDir, listener)
      source.reader.start()
      return source
    }
  }
}
