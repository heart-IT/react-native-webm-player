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
}

- (uint64_t)bytesFedTotal { return _bytesFed.load(); }
- (uint64_t)videoFramesDropped { return _videoFramesDropped.load(); }
- (int)videoWidth { return _videoWidth.load(); }
- (int)videoHeight { return _videoHeight.load(); }

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

  media::demux::WebmDemuxer demuxer;
  std::vector<uint8_t> chunk(kPumpChunkBytes);
  bool firstAudioSeen = false;
  bool firstVideoSeen = false;

  [self reportHealth:@"buffering" detail:@"waiting for stream data"];

  int emptyReads = 0;
  bool stalled = false;

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
      [self reportHealth:@"buffering" detail:@"stream reset"];
    }

    int got = _ring->read(chunk.data(), chunk.size(), kRingReadTimeoutMs);
    if (got <= 0) {
      // read() returns -1 once the ring is shut down or drained at EOS.
      if (got < 0) {
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
      MEDIA_LOG_W("WebmDemuxPump: demux error: %s", result.error.c_str());
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
      // Same last-strong-reference hazard as reportHealth: weak-load on main.
      __weak WebmDemuxPump* weakSelf = self;
      int64_t ptsUs = pkt.ptsUs;
      dispatch_async(dispatch_get_main_queue(), ^{
        WebmDemuxPump* self_ = weakSelf;
        if (!self_) return;
        [self_.delegate demuxPump:self_ didDecodeFirstAudioAtPts:ptsUs];
      });
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
