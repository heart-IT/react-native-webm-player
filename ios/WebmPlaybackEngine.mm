#import "WebmPlaybackEngine.h"
#import "WebmAudioDecoder.h"
#import "WebmVideoDecoder.h"

#import <AudioToolbox/AudioToolbox.h>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "common/MediaLog.h"
#include "common/WebMStreamBuffer.h"
#include "demux/WebmDemuxer.h"

namespace {

// Bytes pulled from the ring per demux pass.
constexpr size_t kPumpChunkBytes = 64 * 1024;

}  // namespace

@implementation WebmPlaybackEngine {
  std::unique_ptr<media::WebMStreamBuffer> _ring;
  AVSampleBufferDisplayLayer* _displayLayer;
  AVSampleBufferAudioRenderer* _audioRenderer;
  AVSampleBufferRenderSynchronizer* _synchronizer;
  WebmVideoDecoder* _videoDecoder;
  WebmAudioDecoder* _audioDecoder;

  std::thread _demuxThread;
  std::atomic<bool> _running;
  std::atomic<bool> _paused;
  std::atomic<bool> _stop;
  // Set by resetStream() on the JS thread, consumed by the demux thread. The
  // demuxer is owned by that thread, so it cannot be reset from here directly.
  std::atomic<bool> _resetRequested;
  std::atomic<bool> _failed;

  std::atomic<uint64_t> _bytesFed;
  std::atomic<uint64_t> _audioPacketsDecoded;
  std::atomic<uint64_t> _videoPacketsDecoded;
  std::atomic<uint64_t> _audioUnderruns;
  std::atomic<uint64_t> _videoFramesDropped;
  std::atomic<int> _videoWidth;
  std::atomic<int> _videoHeight;
  std::atomic<float> _gain;
  std::atomic<bool> _muted;
  std::atomic<float> _playbackRate;

  void (^_healthCallback)(NSString*, NSString*);
  NSString* _lastHealthStatus;  // Main-thread only.
}

- (instancetype)init {
  self = [super init];
  if (!self) return nil;
  _running = false;
  _paused = false;
  _stop = false;
  _resetRequested = false;
  _failed = false;
  _bytesFed = 0;
  _audioPacketsDecoded = 0;
  _videoPacketsDecoded = 0;
  _audioUnderruns = 0;
  _videoFramesDropped = 0;
  _videoWidth = 0;
  _videoHeight = 0;
  _gain = 1.0f;
  _muted = false;
  _playbackRate = 1.0f;
  _lastHealthStatus = @"idle";
  return self;
}

- (void)dealloc {
  [self stop];
}

- (void)setDisplayLayer:(AVSampleBufferDisplayLayer*)layer {
  // CALayer parenting and the synchronizer's renderer set are main-thread only,
  // so dispatch unconditionally — callers include the RN view manager and the
  // demux thread.
  void (^apply)(void) = ^{
    AVSampleBufferDisplayLayer* previous = self->_displayLayer;
    if (previous == layer) return;

    if (self->_synchronizer && previous) {
      [self->_synchronizer removeRenderer:previous
                                   atTime:kCMTimeInvalid
                        completionHandler:nil];
    }
    self->_displayLayer = layer;
    // videoGravity belongs to the view, not the engine.
    if (self->_videoDecoder) [self->_videoDecoder setOutputLayer:layer];
    if (self->_synchronizer && layer) [self->_synchronizer addRenderer:layer];
  };
  if ([NSThread isMainThread]) {
    apply();
  } else {
    dispatch_async(dispatch_get_main_queue(), apply);
  }
}

- (void)detachDisplayLayer:(AVSampleBufferDisplayLayer*)layer {
  void (^apply)(void) = ^{
    if (self->_displayLayer != layer) return;
    [self setDisplayLayer:nil];
  };
  if ([NSThread isMainThread]) {
    apply();
  } else {
    dispatch_async(dispatch_get_main_queue(), apply);
  }
}

- (BOOL)start {
  if (_running) return YES;
  _ring = std::make_unique<media::WebMStreamBuffer>(
      media::WebMStreamBuffer::broadcastCapacityBytes(),
      media::WebMStreamBuffer::broadcastConfig());

  _audioRenderer = [[AVSampleBufferAudioRenderer alloc] init];
  _audioRenderer.muted = _muted.load();
  _audioRenderer.volume = _gain.load();
  _synchronizer = [[AVSampleBufferRenderSynchronizer alloc] init];
  [_synchronizer addRenderer:_audioRenderer];
  if (_displayLayer) [_synchronizer addRenderer:_displayLayer];

  _videoDecoder = [[WebmVideoDecoder alloc] init];
  [_videoDecoder setOutputLayer:_displayLayer];
  _audioDecoder = [[WebmAudioDecoder alloc] init];
  [_audioDecoder setRenderer:_audioRenderer];

  [self setupAudioSession];

  _stop = false;
  _paused = false;
  _failed = false;
  _running = true;
  _demuxThread = std::thread([self] { [self demuxLoop]; });

  [_synchronizer setRate:_playbackRate.load()];
  return YES;
}

- (BOOL)stop {
  if (!_running) return YES;
  _stop = true;
  _running = false;
  if (_ring) _ring->shutdown();
  if (_demuxThread.joinable()) _demuxThread.join();

  if (_synchronizer) {
    [_synchronizer setRate:0];
    if (_audioRenderer) {
      [_synchronizer removeRenderer:_audioRenderer
                             atTime:kCMTimeInvalid
                  completionHandler:nil];
    }
    if (_displayLayer) {
      [_synchronizer removeRenderer:_displayLayer
                             atTime:kCMTimeInvalid
                  completionHandler:nil];
    }
  }
  [_videoDecoder shutdown];
  _videoDecoder = nil;
  _audioDecoder = nil;
  _audioRenderer = nil;
  _synchronizer = nil;
  _ring.reset();
  return YES;
}

- (BOOL)pause {
  _paused = true;
  if (_synchronizer) [_synchronizer setRate:0];
  return YES;
}

- (BOOL)resume {
  _paused = false;
  if (_synchronizer) [_synchronizer setRate:_playbackRate.load()];
  return YES;
}

- (BOOL)isRunning {
  return _running.load();
}

- (BOOL)isPaused {
  return _paused.load();
}

- (size_t)feedData:(const uint8_t*)bytes length:(size_t)length {
  if (!_ring || !_running) return 0;
  // Synthetic chunk boundaries, not cluster-aligned — skip the debug validator.
  size_t wrote = _ring->write(bytes, length, /*isClusterBoundary=*/false);
  _bytesFed.fetch_add(wrote);
  return wrote;
}

- (BOOL)setEndOfStream {
  if (_ring) _ring->setEndOfStream(true);
  return YES;
}

- (BOOL)resetStream {
  // Ring first, so anything the demux thread reads after it observes the flag is
  // already post-clear. The demuxer itself lives on that thread and keeps parse
  // state across feeds; without resetting it, the next stream's EBML header is
  // appended to a demuxer still mid-cluster and every subsequent parse fails.
  if (_ring) _ring->clear();
  _resetRequested.store(true, std::memory_order_release);
  return YES;
}

- (BOOL)setMuted:(BOOL)muted {
  _muted = muted;
  if (_audioRenderer) _audioRenderer.muted = muted;
  return YES;
}

- (BOOL)setGain:(float)gain {
  _gain = gain;
  if (_audioRenderer) _audioRenderer.volume = gain;
  return YES;
}

- (BOOL)setPlaybackRate:(float)rate {
  _playbackRate = rate;
  if (_synchronizer && _running && !_paused) [_synchronizer setRate:rate];
  return YES;
}

- (WebmPlaybackState)playbackState {
  // Checked before _running so a failure survives stop(), as it does on Android.
  // start() clears it.
  if (_failed.load()) return WebmPlaybackStateFailed;
  if (!_running) return WebmPlaybackStateIdle;
  if (_paused) return WebmPlaybackStatePaused;
  if (_videoPacketsDecoded.load() == 0 && _audioPacketsDecoded.load() == 0) {
    return WebmPlaybackStateBuffering;
  }
  return WebmPlaybackStatePlaying;
}

- (WebmPlaybackMetrics)metrics {
  WebmPlaybackMetrics m = {0};
  m.bytesFedTotal = _bytesFed.load();
  m.audioPacketsDecoded = _audioPacketsDecoded.load();
  m.videoPacketsDecoded = _videoPacketsDecoded.load();
  m.audioUnderruns = _audioUnderruns.load();
  m.videoFramesDropped = _videoFramesDropped.load();
  m.audioFramesRecovered = _audioDecoder ? _audioDecoder.framesRecovered : 0;
  m.videoWidth = _videoWidth.load();
  m.videoHeight = _videoHeight.load();
  m.gain = _gain.load();
  m.muted = _muted.load();
  m.playbackRate = _playbackRate.load();
  if (_synchronizer) {
    CMTime t = [_synchronizer currentTime];
    if (CMTIME_IS_VALID(t)) m.currentTimeSeconds = CMTimeGetSeconds(t);
  }
  return m;
}

- (void)fireHealth:(NSString*)status detail:(NSString*)detail {
  // Transition tracking and the callback both run on main, so _lastHealthStatus
  // needs no lock. Health events are rare enough that the hop is free.
  void (^run)(void) = ^{
    void (^cb)(NSString*, NSString*) = self->_healthCallback;
    if (!cb) return;
    if ([self->_lastHealthStatus isEqualToString:status]) return;
    self->_lastHealthStatus = status;
    cb(status, detail ?: @"");
  };
  if ([NSThread isMainThread]) {
    run();
  } else {
    dispatch_async(dispatch_get_main_queue(), run);
  }
}

- (void)setHealthCallback:(void (^)(NSString*, NSString*))callback {
  _healthCallback = [callback copy];
}

- (void)setupAudioSession {
  AVAudioSession* session = [AVAudioSession sharedInstance];
  NSError* err = nil;
  if (![session setCategory:AVAudioSessionCategoryPlayback
                       mode:AVAudioSessionModeDefault
                    options:0
                      error:&err]) {
    MEDIA_LOG_E("WebmPlaybackEngine: audio session category failed: %s",
                err.localizedDescription.UTF8String);
  }
  if (![session setActive:YES error:&err]) {
    MEDIA_LOG_E("WebmPlaybackEngine: audio session activate failed: %s",
                err.localizedDescription.UTF8String);
  }
}

#pragma mark - Demux loop

- (void)demuxLoop {
  pthread_setname_np("webmplayer.demux");
  if (!_ring) return;

  media::demux::WebmDemuxer demuxer;
  std::vector<uint8_t> chunk(kPumpChunkBytes);
  bool firstAudioSeen = false;
  bool firstVideoSeen = false;

  [self fireHealth:@"buffering" detail:@"waiting for stream data"];

  while (!_stop.load()) {
    // Observed before reading, so every byte taken afterwards belongs to the new
    // stream. resetStream() clears the ring before raising this.
    if (_resetRequested.exchange(false, std::memory_order_acquire)) {
      demuxer.reset();
      [_audioDecoder reset];
      firstAudioSeen = false;
      firstVideoSeen = false;
      _failed.store(false);
      [self fireHealth:@"buffering" detail:@"stream reset"];
    }

    int got = _ring->read(chunk.data(), chunk.size(), 50);
    if (got <= 0) {
      // read() returns -1 once the ring is shut down or drained at EOS.
      if (got < 0) {
        [self fireHealth:@"ended" detail:@"end of stream"];
        break;
      }
      continue;
    }

    // Packets point into the demuxer's internal buffer and are invalidated by
    // the next feedData(), so everything is consumed before looping.
    const auto& result =
        demuxer.feedData(chunk.data(), static_cast<size_t>(got));

    if (!result.audioPackets.empty()) {
      auto info = demuxer.trackInfoSnapshot();
      if ([_audioDecoder configureWithSampleRate:info.audioSampleRate
                                        channels:info.audioChannels]) {
        for (const auto& pkt : result.audioPackets) {
          if ([_audioDecoder submitPacket:pkt.data
                                   length:pkt.size
                                    ptsUs:pkt.ptsUs
                               durationUs:pkt.durationUs]) {
            if (!firstAudioSeen) {
              firstAudioSeen = true;
              [self fireHealth:@"playing" detail:@"first audio frame"];
            }
          }
        }
        _audioPacketsDecoded.store(_audioDecoder.packetsDecoded);
        _audioUnderruns.store(_audioDecoder.underruns);
      }
    }

    if (!result.videoPackets.empty()) {
      auto info = demuxer.trackInfoSnapshot();
      _videoWidth.store(info.videoWidth);
      _videoHeight.store(info.videoHeight);
      // The container knows the frame size, so a VP9 header the parser cannot
      // read no longer means no video at all.
      [_videoDecoder setContainerWidth:info.videoWidth height:info.videoHeight];
      for (const auto& pkt : result.videoPackets) {
        BOOL ok = [_videoDecoder submitFrame:pkt.data
                                      length:pkt.size
                                       ptsUs:pkt.ptsUs
                                       isKey:pkt.isKeyFrame ? YES : NO];
        if (ok) {
          _videoPacketsDecoded.fetch_add(1);
          if (!firstVideoSeen) {
            firstVideoSeen = true;
            [self fireHealth:@"playing" detail:@"first video frame"];
          }
        } else {
          _videoFramesDropped.fetch_add(1);
        }
      }
    }

    if (!result.error.empty()) {
      MEDIA_LOG_W("WebmPlaybackEngine: demux error: %s", result.error.c_str());
    }

    // A transient parse error is normal on a lossy feed and is logged above.
    // The demuxer's terminal state is not: nothing downstream recovers from it,
    // so surface it instead of looping silently.
    if (demuxer.parseState() == media::demux::ParseState::Error) {
      if (!_failed.exchange(true)) {
        MEDIA_LOG_E("WebmPlaybackEngine: demuxer entered a terminal parse error");
        [self fireHealth:@"failed" detail:@"demuxer parse failure"];
      }
    }
  }
}

@end
