// Remuxes a vcr-core reader (a VCRInputFormat over a Hypercore media log) into
// muxed WebM, handing out bytes as ffmpeg's muxer flushes them.
const ffmpeg = require('bare-ffmpeg')

// avio hands bytes to onwrite when this buffer fills or a packet is flushed.
// Small, so a packet reaches the player without waiting for a large buffer.
const kAvioBufferBytes = 4096

/**
 * @param input a VCRInputFormat that has already been sought
 * @param write `(chunk: Buffer) => Promise | null`; a promise means "wait for
 *   room before reading the next packet", which is how the consumer's
 *   backpressure reaches the Hypercore reader
 * @param shouldStop polled before every packet; true ends the loop early
 * @returns 'ended' at end of stream, 'stopped' when shouldStop fired
 */
module.exports = async function remuxWebm(input, write, shouldStop) {
  const pending = []
  const io = new ffmpeg.IOContext(kAvioBufferBytes, {
    // avio reuses its buffer after this returns, so copy before queuing.
    onwrite: (view) => {
      const wait = write(Buffer.from(view))
      if (wait) pending.push(wait)
    },
  })
  const out = new ffmpeg.OutputFormatContext('webm', io)
  for (const stream of input.streams) {
    const outStream = out.createStream()
    stream.toNativeStream(outStream)
    if (stream.type === ffmpeg.constants.mediaTypes.VIDEO) {
      outStream.codecParameters.frameRate = stream.avgFramerate
    }
  }
  // A live join lands mid-GOP. Frames before the first keyframe cannot be
  // decoded by anyone, so the muxed stream starts at that keyframe; a stream
  // without video starts at once. The header is written then too, so the
  // player does not see a header followed by seconds of nothing.
  const videoIndex = input.streams.findIndex(
    (s) => s.type === ffmpeg.constants.mediaTypes.VIDEO
  )
  let started = false
  const start = () => {
    const options = new ffmpeg.Dictionary()
    // Flush avio after every packet: the consumer is a live player, not a file.
    options.set('flush_packets', '1')
    out.writeHeader(options)
    options.destroy()
    started = true
  }

  const packet = new ffmpeg.Packet()
  try {
    while (!shouldStop()) {
      if (!(await input.readFrame(packet))) {
        if (!started) start()
        out.writeTrailer()
        await drain(pending)
        return 'ended'
      }
      if (!started) {
        if (
          videoIndex >= 0 &&
          !(packet.streamIndex === videoIndex && packet.isKeyframe)
        ) {
          packet.unref()
          continue
        }
        start()
      }
      out.writeFrame(packet)
      packet.unref()
      await drain(pending)
    }
    return 'stopped'
  } finally {
    packet.destroy()
    out.destroy()
  }
}

async function drain(pending) {
  if (pending.length === 0) return
  await Promise.all(pending.splice(0))
}
