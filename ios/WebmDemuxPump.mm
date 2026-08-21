#import "WebmDemuxPump.h"

#import "WebmAudioDecoder.h"
#import "WebmVideoDecoder.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "common/MediaLog.h"
#include "common/WebMStreamBuffer.h"
#include "demux/WebmDemuxer.h"

namespace {

// Bytes pulled from the ring per demux pass.
constexpr size_t kPumpChunkBytes = 64 * 1024;

// Blocking read timeout. Bounds how long stop() waits for the thread to notice
// the shutdown, since the ring wakes readers on shutdown() anyway.
constexpr int kRingReadTimeoutMs = 50;

// Consecutive empty reads before the pump reports a stall: 40 × 50 ms = 2 s.
// A live feed that goes quiet for 2 s is already a visible stall.
constexpr int kStallReadTimeouts = 40;

// Per-pass logs sample every 512th occurrence: sustained corruption reports an
// error on every demux pass, ~10+/s at stream bitrate.
constexpr uint64_t kErrorLogMask = 511;

}  // namespace

@interface WebmDemuxPump ()
- (void)run;
- (void)consumeAudio:(const std::vector<media::demux::AudioPacket>&)packets
             demuxer:(const media::demux::WebmDemuxer&)demuxer
      firstAudioSeen:(bool*)firstAudioSeen;
- (void)consumeVideo:(const std::vector<media::demux::VideoPacket>&)packets
             demuxer:(const media::demux::WebmDemuxer&)demuxer
      firstVideoSeen:(bool*)firstVideoSeen;
@end

@implementation WebmDemuxPump {
  std::unique_ptr<media::WebMStreamBuffer> _ring;
  WebmAudioDecoder* _audioDecoder;
  WebmVideoDecoder* _videoDecoder;

  std::thread _thread;
  // True while run() is executing. The thread exits on end-of-stream, so a
  // later requestReset must know whether anyone is left to consume the flag.
  std::atomic<bool> _threadLive;
  std::atomic<bool> _stop;
  std::atomic<bool> _paused;
  // Set by requestReset() on the JS thread, consumed by the demux thread.
  std::atomic<bool> _resetRequested;
  std::atomic<bool> _failed;

  std::atomic<uint64_t> _bytesFed;
  std::atomic<uint64_t> _videoFramesDropped;
  std::atomic<int> _videoWidth;
  std::atomic<int> _videoHeight;
}

- (instancetype)init {
  self = [super init];
  if (!self) return nil;
  _threadLive = false;
  _stop = false;
  _paused = false;
  _resetRequested = false;
  _failed = false;
  _bytesFed = 0;
  _videoFramesDropped = 0;
  _videoWidth = 0;
  _videoHeight = 0;
  return self;
}

- (void)dealloc {
  [self stop];
}

- (void)startWithAudioDecoder:(WebmAudioDecoder*)audioDecoder
                 videoDecoder:(WebmVideoDecoder*)videoDecoder {
  if (_thread.joinable()) return;
  _audioDecoder = audioDecoder;
  _videoDecoder = videoDecoder;
  _ring = std::make_unique<media::WebMStreamBuffer>(
      media::WebMStreamBuffer::broadcastCapacityBytes(),
      media::WebMStreamBuffer::broadcastConfig());
  _stop = false;
  _failed = false;
  // The pump object is reused across start/stop cycles: a pause or pending
  // reset from the previous session must not leak into this one, or the new
  // thread parks forever on a flag nothing will clear.
  _paused = false;
  _resetRequested = false;
  _thread = std::thread([self] { [self run]; });
}

- (void)stop {
  _stop = true;
  if (_ring) _ring->shutdown();
  if (_thread.joinable()) _thread.join();
  _ring.reset();
  // Released only after the thread is joined — it dereferences both.
  _audioDecoder = nil;
  _videoDecoder = nil;
}

- (size_t)feedData:(const uint8_t*)bytes length:(size_t)length {
  if (!_ring) return 0;
  if (_resetRequested.load(std::memory_order_relaxed)) [self restartIfExited];
  // Synthetic chunk boundaries, not cluster-aligned — skip the debug validator.
  size_t wrote = _ring->write(bytes, length, /*isClusterBoundary=*/false);
  _bytesFed.fetch_add(wrote);
  return wrote;
}

- (void)setPaused:(BOOL)paused {
  _paused.store(paused);
}

- (void)setEndOfStream {
  if (_ring) _ring->setEndOfStream(true);
}

- (void)requestReset {
  // Ring first, so anything the demux thread reads after it observes the flag is
  // already post-clear. The demuxer itself lives on that thread and keeps parse
  // state across feeds; without resetting it, the next stream's EBML header is
  // appended to a demuxer still mid-cluster and every subsequent parse fails.
  if (_ring) _ring->clear();
  _resetRequested.store(true, std::memory_order_release);
  [self restartIfExited];
}

// The demux thread exits at end-of-stream, so a post-EOS reset (broadcast
// reconnect) has nobody left to consume the flag — restart the thread. Runs on
// the JS thread only, the same thread as start/stop/requestReset/feedData, so
// there is no lifecycle race with those. A thread observed live here can still
// be mid-exit; the same check in feedData retries on the next chunk.
- (void)restartIfExited {
  if (!_ring || _stop.load() || _threadLive.load()) return;
  if (_thread.joinable()) _thread.join();
  _thread = std::thread([self] { [self run]; });
}

- (uint64_t)bytesFedTotal { return _bytesFed.load(); }

- (uint64_t)bytesDropped {
  return _ring ? _ring->getStats().droppedBytes : 0;
}

- (double)ringFillRatio {
  if (!_ring) return 0.0;
  auto s = _ring->getStats();
  return s.capacityBytes ? static_cast<double>(s.currentSizeBytes) / s.capacityBytes : 0.0;
}

- (uint64_t)videoFramesDropped { return _videoFramesDropped.load(); }
- (int)videoWidth { return _videoWidth.load(); }
- (int)videoHeight { return _videoHeight.load(); }

// Starts the engine's media clock. Same last-strong-reference hazard as
// reportHealth: weak-load the delegate on main.
- (void)notifyFirstSampleAtPts:(int64_t)ptsUs {
  __weak WebmDemuxPump* weakSelf = self;
  dispatch_async(dispatch_get_main_queue(), ^{
    WebmDemuxPump* self_ = weakSelf;
    if (!self_) return;
    [self_.delegate demuxPump:self_ didDecodeFirstSampleAtPts:ptsUs];
  });
}

- (void)reportHealth:(NSString*)status detail:(NSString*)detail {
  // Async to main, weak-loading the delegate there rather than here: loading a
  // weak engine reference on the demux thread can take its last strong
  // reference, and engine dealloc → stop → join would then join the very
  // thread this runs on.
  __weak WebmDemuxPump* weakSelf = self;
  dispatch_async(dispatch_get_main_queue(), ^{
    WebmDemuxPump* self_ = weakSelf;
    if (!self_) return;
    [self_.delegate demuxPump:self_ didReportHealth:status detail:detail];
  });
}

#pragma mark - Demux thread

- (void)run {
  pthread_setname_np("webmplayer.demux");
  if (!_ring) return;
  _threadLive.store(true);

  media::demux::WebmDemuxer demuxer;
  std::vector<uint8_t> chunk(kPumpChunkBytes);
  bool firstAudioSeen = false;
  bool firstVideoSeen = false;

  [self reportHealth:@"buffering" detail:@"waiting for stream data"];

  int emptyReads = 0;
  bool stalled = false;
  uint64_t demuxErrorLogs = 0;

  // The pool drains every pass: this thread sends Objective-C messages whose
  // autoreleased returns would otherwise have no pool on a bare std::thread.
  while (!_stop.load()) {
    @autoreleasepool {
    if (_paused.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(kRingReadTimeoutMs));
      continue;
    }

    // Observed before reading, so every byte taken afterwards belongs to the new
    // stream. requestReset() clears the ring before raising this.
    if (_resetRequested.exchange(false, std::memory_order_acquire)) {
      demuxer.reset();
      [_audioDecoder reset];
      firstAudioSeen = false;
      firstVideoSeen = false;
      _failed.store(false);
      // The engine flushes the renderers and re-anchors the clock — same
      // weak-load-on-main pattern as reportHealth.
      __weak WebmDemuxPump* weakSelf = self;
      dispatch_async(dispatch_get_main_queue(), ^{
        WebmDemuxPump* self_ = weakSelf;
        if (!self_) return;
        [self_.delegate demuxPumpDidResetStream:self_];
      });
      [self reportHealth:@"buffering" detail:@"stream reset"];
    }

    int got = _ring->read(chunk.data(), chunk.size(), kRingReadTimeoutMs);
    if (got <= 0) {
      // read() returns -1 once the ring is shut down or drained at EOS.
      if (got < 0) {
        // A shutdown-woken read exits silently: that "end" belongs to stop(),
        // not the broadcast, and Android's stop() emits nothing either.
        if (_stop.load()) break;
        // A reset that raced the EOS drain re-arms the ring; consume it at the
        // loop top instead of exiting with the flag stranded.
        if (_resetRequested.load(std::memory_order_acquire)) continue;
        [self reportHealth:@"ended" detail:@"end of stream"];
        break;
      }
      // A mid-stream stall was invisible before: the state machine latched
      // "playing" at the first frame and nothing ever reported the feed going
      // quiet, so a stalled stream looked identical to a playing one. Android
      // reports rebuffering; this is the iOS side of that contract.
      if (++emptyReads >= kStallReadTimeouts && !stalled) {
        stalled = true;
        [self reportHealth:@"buffering" detail:@"stream stalled"];
      }
      continue;
    }
    emptyReads = 0;
    if (stalled) {
      stalled = false;
      if (firstAudioSeen || firstVideoSeen) {
        [self reportHealth:@"playing" detail:@"stream resumed"];
      }
    }

    // Packets point into the demuxer's internal buffer and are invalidated by
    // the next feedData(), so everything is consumed before looping.
    const auto& result =
        demuxer.feedData(chunk.data(), static_cast<size_t>(got));

    if (!result.audioPackets.empty()) {
      [self consumeAudio:result.audioPackets
                 demuxer:demuxer
           firstAudioSeen:&firstAudioSeen];
    }
    if (!result.videoPackets.empty()) {
      [self consumeVideo:result.videoPackets
                 demuxer:demuxer
          firstVideoSeen:&firstVideoSeen];
    }

    if (!result.error.empty()) {
      // Sustained corruption reports an error every pass; log a sample, not all.
      if ((demuxErrorLogs++ & kErrorLogMask) == 0) {
        MEDIA_LOG_W("WebmDemuxPump: demux error: %s (%llu so far)",
                    result.error.c_str(),
                    static_cast<unsigned long long>(demuxErrorLogs));
      }
    }

    // A transient parse error is normal on a lossy feed and is logged above.
    // The demuxer's terminal state is not: nothing downstream recovers from it,
    // so surface it instead of looping silently.
    if (demuxer.parseState() == media::demux::ParseState::Error) {
      if (!_failed.exchange(true)) {
        MEDIA_LOG_E("WebmDemuxPump: demuxer entered a terminal parse error");
        [self reportHealth:@"failed" detail:@"demuxer parse failure"];
      }
    }
    }  // @autoreleasepool
  }
  _threadLive.store(false);
}

- (void)consumeAudio:(const std::vector<media::demux::AudioPacket>&)packets
             demuxer:(const media::demux::WebmDemuxer&)demuxer
      firstAudioSeen:(bool*)firstAudioSeen {
  auto info = demuxer.trackInfoSnapshot();
  if (![_audioDecoder configureWithSampleRate:info.audioSampleRate
                                     channels:info.audioChannels
                                     opusHead:info.audioCodecPrivate.empty()
                                                  ? nullptr
                                                  : info.audioCodecPrivate.data()
                                 opusHeadSize:info.audioCodecPrivate.size()]) {
    // The track parameters come from the stream and will not change until a
    // reset, so this failure is permanent: without surfacing it the clock
    // never starts, queued video never presents, and health sits at
    // "buffering" forever over a silently dead pipeline.
    if (!_failed.exchange(true)) {
      MEDIA_LOG_E("WebmDemuxPump: audio decoder configuration failed permanently");
      [self reportHealth:@"failed" detail:@"audio decoder configuration failed"];
    }
    return;
  }
  for (const auto& pkt : packets) {
    if (![_audioDecoder submitPacket:pkt.data
                              length:pkt.size
                               ptsUs:pkt.ptsUs
                          durationUs:pkt.durationUs]) {
      continue;
    }
    if (!*firstAudioSeen) {
      *firstAudioSeen = true;
      [self notifyFirstSampleAtPts:pkt.ptsUs];
      [self reportHealth:@"playing" detail:@"first audio frame"];
    }
  }
}

- (void)consumeVideo:(const std::vector<media::demux::VideoPacket>&)packets
             demuxer:(const media::demux::WebmDemuxer&)demuxer
      firstVideoSeen:(bool*)firstVideoSeen {
  auto info = demuxer.trackInfoSnapshot();
  _videoWidth.store(info.videoWidth);
  _videoHeight.store(info.videoHeight);
  // The container knows the frame size, so a VP9 header the parser cannot read
  // no longer means no video at all.
  [_videoDecoder setContainerWidth:info.videoWidth height:info.videoHeight];

  for (const auto& pkt : packets) {
    BOOL ok = [_videoDecoder submitFrame:pkt.data
                                  length:pkt.size
                                   ptsUs:pkt.ptsUs
                                   isKey:pkt.isKeyFrame ? YES : NO];
    if (ok) {
      if (!*firstVideoSeen) {
        *firstVideoSeen = true;
        // A video-only stream has no first audio packet to anchor the clock;
        // without this the synchronizer stays at rate 0 and nothing presents.
        if (info.audioTrackNum < 0) [self notifyFirstSampleAtPts:pkt.ptsUs];
        [self reportHealth:@"playing" detail:@"first video frame"];
      }
    } else {
      _videoFramesDropped.fetch_add(1);
      if (!*firstVideoSeen && !_videoDecoder.hardwareDecodeSupported) {
        *firstVideoSeen = true;  // report once, not per frame
        [self reportHealth:@"playing"
                    detail:@"audio only — no VP9 decoder on this device"];
      }
    }
  }
}

@end
