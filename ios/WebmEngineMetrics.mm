#import "WebmEngineMetrics.h"

#import "WebmAudioDecoder.h"
#import "WebmDemuxPump.h"
#import "WebmVideoDecoder.h"

WebmPlaybackMetrics WebmCollectMetrics(WebmDemuxPump* pump,
                                       WebmAudioDecoder* audio,
                                       WebmVideoDecoder* video,
                                       float gain, bool muted, float rate) {
  WebmPlaybackMetrics m = {0};
  m.bytesFedTotal = pump.bytesFedTotal;
  // Read from the decoders rather than mirroring into atomics on the demux
  // thread: the renderer keeps draining after the last packet is fed, so a
  // mirror updated only while demuxing freezes mid-playback.
  m.audioPacketsDecoded = audio ? audio.packetsDecoded : 0;
  // Frames that reached the display layer, not frames handed to VideoToolbox.
  // Android reports renderedOutputBufferCount, so submitting-but-never-showing
  // would otherwise look identical to playing on one platform and not the other.
  m.videoPacketsDecoded = video ? video.framesPresented : 0;
  // Renderer starvation is not observable through AVSampleBufferAudioRenderer,
  // so iOS reports 0 rather than a different quantity under the same name —
  // Android reports ExoPlayer's genuine underrun events.
  m.audioUnderruns = 0;
  // Submit-stage failures plus renderer-stage discards: Android reports
  // droppedBufferCount (renderer stage), and a wedged layer must not read as
  // "dropped 0" here while its drops climb only in a log.
  m.videoFramesDropped = pump.videoFramesDropped + (video ? video.pendingCapDrops : 0);
  m.audioFramesRecovered = audio ? audio.framesRecovered : 0;
  m.videoWidth = pump.videoWidth;
  m.videoHeight = pump.videoHeight;
  m.gain = gain;
  m.muted = muted;
  m.playbackRate = rate;
  // The synchronizer's clock keeps advancing once started, so after a stream
  // ends it reports wall-clock rather than position. The last presented
  // timestamp is the stream's own timeline, and matches what Android reports.
  int64_t lastPts = audio ? audio.lastPresentedPtsUs : -1;
  if (lastPts >= 0) m.currentTimeSeconds = static_cast<double>(lastPts) / 1000000.0;
  return m;
}
