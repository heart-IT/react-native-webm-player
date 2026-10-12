#import "WebmDemuxPump.h"

#import "WebmAudioDecoder.h"
#import "WebmPlaybackPolicy.h"
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

// A device log has no metrics view: one summary line this often is how a
// run's counters are read back.
constexpr int64_t kSummaryIntervalMs = 5000;

// Video drops are logged at these milestones with the decoder's state, which
// says whether they were frames before a keyframe or something worse.
constexpr uint64_t kDropLogEvery = 100;

// The demux thread waits while a decoder holds this much decoded media, so
// the playout margin accumulates in the ring as compressed bytes rather than
// overflowing the decoders' queues. Below each decoder's cap, leaving room
// for frames VideoToolbox still has in flight.
constexpr size_t kVideoPacingFrames = 20;
constexpr size_t kAudioPacingBuffers = 60;
constexpr int kPacingSleepMs = 5;

}  // namespace

@interface WebmDemuxPump ()
- (void)run;
- (BOOL)prepareAudio:(const media::demux::WebmDemuxer&)demuxer;
- (void)submitAudio:(const media::demux::AudioPacket&)pkt firstSeen:(bool*)firstSeen;
- (void)prepareVideo:(const media::demux::WebmDemuxer&)demuxer;
- (void)submitVideo:(const media::demux::VideoPacket&)pkt
            demuxer:(const media::demux::WebmDemuxer&)demuxer
          firstSeen:(bool*)firstSeen;
@end

@implementation WebmDemuxPump {
  std::unique_ptr<media::WebMStreamBuffer> _ring;
  WebmAudioDecoder* _audioDecoder;
  WebmVideoDecoder* _videoDecoder;

  std::thread _thread;
  std::atomic<bool> _stop;
  // Set by requestReset() on the engine's control thread, consumed by the
  // demux thread.
  std::atomic<bool> _resetRequested;
  std::atomic<bool> _failed;

  std::atomic<uint64_t> _bytesFed;
  std::atomic<int64_t> _lastAudioPtsUs;  // newest audio packet decoded
  std::atomic<uint64_t> _videoFramesDropped;
  std::atomic<int> _videoWidth;
  std::atomic<int> _videoHeight;
}

- (instancetype)init {
  self = [super init];
  if (!self) return nil;
  _stop = false;
  _resetRequested = false;
  _failed = false;
  _bytesFed = 0;
  _lastAudioPtsUs = -1;
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
  _ring = std::make_unique<media::WebMStreamBuffer>(kRingCapacityBytes);
  _stop = false;
  _failed = false;
  // The pump object is reused across start/stop cycles: a pending reset from
  // the previous session must not leak into this one.
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
  size_t wrote = _ring->write(bytes, length);
  _bytesFed.fetch_add(wrote);
  return wrote;
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

- (uint64_t)bytesReceived { return _bytesFed.load(); }

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

  media::demux::WebmDemuxer demuxer;
  std::vector<uint8_t> chunk(kPumpChunkBytes);
  bool firstAudioSeen = false;
  bool firstVideoSeen = false;

  [self reportHealth:@"buffering" detail:@"waiting for stream data"];

  int emptyReads = 0;
  bool stalled = false;
  // Quiet before the first byte is the swarm connecting, not a stall.
  bool receivedData = false;
  uint64_t demuxErrorLogs = 0;
  int64_t lastSummaryMs = media::log::detail::millisSinceStart();
  // Checked every pass, not every summary: the loop idles in 50 ms reads, so a
  // renderer running dry is seen within that. One underrun per dry spell.
  bool dry = false;
  uint64_t underruns = 0;

  // The pool drains every pass: this thread sends Objective-C messages whose
  // autoreleased returns would otherwise have no pool on a bare std::thread.
  while (!_stop.load()) {
    @autoreleasepool {
    int64_t nowMs = media::log::detail::millisSinceStart();
    // How far the newest decoded audio runs ahead of the clock; the rest of
    // the live delay waits in the ring as bytes. At or below zero the renderer
    // has run dry.
    double clock = _clock ? _clock() : -1.0;
    int64_t lastPts = _lastAudioPtsUs.load();
    bool clocked = clock >= 0 && lastPts >= 0;
    double ahead = clocked ? lastPts / 1000000.0 - clock : 0.0;
    if (clocked && ahead <= 0.0 && !dry) {
      MEDIA_LOG_W("WebmDemuxPump: renderer ran dry, underrun %llu",
                  static_cast<unsigned long long>(++underruns));
    }
    dry = clocked && ahead <= 0.0;
    if (nowMs - lastSummaryMs >= kSummaryIntervalMs) {
      lastSummaryMs = nowMs;
      MEDIA_LOG_I("WebmDemuxPump: %llu B in, audio %llu decoded, video %llu presented %llu dropped, "
                  "ring %.1f%%, window %zu KB, pending a%zu v%zu, ahead %.2fs, underruns %llu, %s",
                  static_cast<unsigned long long>(_bytesFed.load()),
                  static_cast<unsigned long long>(_audioDecoder.packetsDecoded),
                  static_cast<unsigned long long>(_videoDecoder.framesPresented),
                  static_cast<unsigned long long>(_videoFramesDropped.load()),
                  100.0 * _ring->sizeBytes() / _ring->capacity(),
                  demuxer.bufferBytes() / 1024,
                  _audioDecoder.pendingBuffers, _videoDecoder.pendingFrames, ahead,
                  static_cast<unsigned long long>(underruns),
                  [_videoDecoder leadReport].UTF8String);
      WebmVideoDecoder* video = _videoDecoder;
      dispatch_async(dispatch_get_main_queue(), ^{ [video logLayerPerformance]; });
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
        [self reportHealth:@"ended" detail:@"end of stream"];
        break;
      }
      // Android reports rebuffering; this is the iOS side of that contract.
      if (receivedData && ++emptyReads >= kStallReadTimeouts && !stalled) {
        stalled = true;
        [self reportHealth:@"buffering" detail:@"stream stalled"];
      }
      continue;
    }
    receivedData = true;
    emptyReads = 0;
    if (stalled) {
      stalled = false;
      if (firstAudioSeen || firstVideoSeen) {
        [self reportHealth:@"playing" detail:@"stream resumed"];
      }
    }

    // Packets point into the demuxer's internal buffer and are invalidated by
    // the next demuxer feed, so everything is consumed before looping.
    const auto& result =
        demuxer.feedData(chunk.data(), static_cast<size_t>(got));

    const auto& audio = result.audioPackets;
    const auto& video = result.videoPackets;
    // Submitted in presentation order across both tracks: a chunk holds
    // seconds of each, and feeding all its audio first, paced on the renderer,
    // handed the decoder that chunk's first video frames after their time.
    size_t ai = (!audio.empty() && ![self prepareAudio:demuxer]) ? audio.size() : 0;
    if (!video.empty()) [self prepareVideo:demuxer];
    size_t vi = 0;
    while (ai < audio.size() || vi < video.size()) {
      if (vi == video.size() || (ai < audio.size() && audio[ai].ptsUs <= video[vi].ptsUs)) {
        [self submitAudio:audio[ai++] firstSeen:&firstAudioSeen];
      } else {
        [self submitVideo:video[vi++] demuxer:demuxer firstSeen:&firstVideoSeen];
      }
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
}

// Holds the demux thread while a decoder's queue is at its pacing limit. A
// stop or reset ends the wait; the packet is then submitted or discarded by
// the caller's normal path.
- (void)paceOn:(size_t (^)(void))pending limit:(size_t)limit {
  while (pending() >= limit && !_stop.load() &&
         !_resetRequested.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kPacingSleepMs));
  }
}

- (BOOL)prepareAudio:(const media::demux::WebmDemuxer&)demuxer {
  const auto& info = demuxer.trackInfo();
  if ([_audioDecoder configureWithSampleRate:info.audioSampleRate
                                    channels:info.audioChannels]) {
    return YES;
  }
  // The track parameters will not change until a reset, so this is permanent;
  // unreported, the clock never starts and health sits at "buffering" forever.
  if (!_failed.exchange(true)) {
    MEDIA_LOG_E("WebmDemuxPump: audio decoder configuration failed permanently");
    [self reportHealth:@"failed" detail:@"audio decoder configuration failed"];
  }
  return NO;
}

- (void)submitAudio:(const media::demux::AudioPacket&)pkt firstSeen:(bool*)firstSeen {
  [self paceOn:^size_t { return self->_audioDecoder.pendingBuffers; } limit:kAudioPacingBuffers];
  if (![_audioDecoder submitPacket:pkt.data length:pkt.size ptsUs:pkt.ptsUs]) return;
  _lastAudioPtsUs.store(pkt.ptsUs);
  if (!*firstSeen) {
    *firstSeen = true;
    [self notifyFirstSampleAtPts:pkt.ptsUs];
    [self reportHealth:@"playing" detail:@"first audio frame"];
  }
}

- (void)prepareVideo:(const media::demux::WebmDemuxer&)demuxer {
  const auto& info = demuxer.trackInfo();
  _videoWidth.store(info.videoWidth);
  _videoHeight.store(info.videoHeight);
}

- (void)submitVideo:(const media::demux::VideoPacket&)pkt
            demuxer:(const media::demux::WebmDemuxer&)demuxer
          firstSeen:(bool*)firstSeen {
  [self paceOn:^size_t { return self->_videoDecoder.pendingFrames; } limit:kVideoPacingFrames];
  BOOL ok = [_videoDecoder submitFrame:pkt.data
                                length:pkt.size
                                 ptsUs:pkt.ptsUs
                                 isKey:pkt.isKeyFrame ? YES : NO];
  if (ok) {
    if (!*firstSeen) {
      *firstSeen = true;
      // A video-only stream has no first audio packet to anchor the clock;
      // without this the synchronizer stays at rate 0 and nothing presents.
      if (demuxer.trackInfo().audioTrackNum < 0) [self notifyFirstSampleAtPts:pkt.ptsUs];
      [self reportHealth:@"playing" detail:@"first video frame"];
    }
    return;
  }
  uint64_t dropped = _videoFramesDropped.fetch_add(1) + 1;
  if (dropped % kDropLogEvery == 0) {
    MEDIA_LOG_W("WebmDemuxPump: %llu video frames dropped at submit, session %s",
                static_cast<unsigned long long>(dropped),
                _videoDecoder.hasSession ? "live" : "none");
  }
  if (!*firstSeen && !_videoDecoder.hardwareDecodeSupported) {
    *firstSeen = true;  // report once, not per frame
    [self reportHealth:@"playing" detail:@"audio only — no VP9 decoder on this device"];
  }
}

@end
