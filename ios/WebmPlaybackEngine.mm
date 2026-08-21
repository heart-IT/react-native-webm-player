#import "WebmPlaybackEngine.h"
#import "WebmAudioDecoder.h"
#import "WebmAudioSessionController.h"
#import "WebmDemuxPump.h"
#import "WebmEngineMetrics.h"
#import "WebmInterruptionObserver.h"
#import "WebmLifecycleObserver.h"
#import "WebmVideoDecoder.h"

#import <UIKit/UIKit.h>
#include <algorithm>
#include <atomic>

#include "common/MediaLog.h"

namespace {
// Spec ranges (webm-player.nitro.ts); Android clamps to the same values, so an
// out-of-range value must read back identically on both platforms.
constexpr float kMinGain = 0.0f;
// 1.0, not 2.0: media3 clamps volume to [0,1], so >1 was iOS-only behavior.
constexpr float kMaxGain = 1.0f;
constexpr float kMinPlaybackRate = 0.5f;
constexpr float kMaxPlaybackRate = 2.0f;

void RunOnMain(void (^block)(void)) {
  if ([NSThread isMainThread]) {
    block();
  } else {
    dispatch_async(dispatch_get_main_queue(), block);
  }
}
}  // namespace

@interface WebmPlaybackEngine () <WebmDemuxPumpDelegate>
@end

@implementation WebmPlaybackEngine {
  WebmDemuxPump* _pump;
  WebmAudioSessionController* _session;
  WebmLifecycleObserver* _lifecycle;
  WebmInterruptionObserver* _interruption;
  AVSampleBufferDisplayLayer* _displayLayer;
  AVSampleBufferAudioRenderer* _audioRenderer;
  AVSampleBufferRenderSynchronizer* _synchronizer;
  WebmVideoDecoder* _videoDecoder;
  WebmAudioDecoder* _audioDecoder;

  std::atomic<bool> _running;
  std::atomic<bool> _paused;
  // Backgrounded is kept apart from _paused: resuming must not start playing a
  // stream the user had explicitly paused before backgrounding.
  std::atomic<bool> _backgrounded;
  // Active audio-session interruption (phone call): resume() and rate changes
  // must not restart the host clock while the system holds the session.
  std::atomic<bool> _interrupted;
  // Latched from the pump's terminal-parse-error health event, so a failure
  // survives stop() as it does on Android. start() clears it.
  std::atomic<bool> _failed;
  // The synchronizer's clock is the media timeline. Starting it before any
  // media exists makes it run ahead of the stream, so it is started at the
  // first sample's presentation time instead.
  std::atomic<bool> _clockStarted;

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
  _backgrounded = false;
  _failed = false;
  _clockStarted = false;
  _gain = 1.0f;
  _muted = false;
  _playbackRate = 1.0f;
  _interrupted = false;
  _lastHealthStatus = @"idle";
  _session = [[WebmAudioSessionController alloc] init];
  // Outlives start/stop cycles so its cumulative counters do too.
  _pump = [[WebmDemuxPump alloc] init];
  _pump.delegate = self;

  __weak WebmPlaybackEngine* weakSelf = self;
  _lifecycle = [[WebmLifecycleObserver alloc]
      initWithOnBackground:^{ [weakSelf suspendForBackground]; }
              onForeground:^{ [weakSelf resumeFromForeground]; }];
  _interruption = [[WebmInterruptionObserver alloc]
      initWithOnBegan:^{ [weakSelf interruptionBegan]; }
              onEnded:^(BOOL shouldResume) {
                [weakSelf interruptionEnded:shouldResume];
              }];
  return self;
}

- (void)dealloc {
  [self stop];
}

- (void)setDisplayLayer:(AVSampleBufferDisplayLayer*)layer {
  // CALayer parenting and the synchronizer's renderer set are main-thread only,
  // so dispatch unconditionally — callers include the RN view manager and the
  // demux thread.
  RunOnMain(^{
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
  });
}

- (void)detachDisplayLayer:(AVSampleBufferDisplayLayer*)layer {
  RunOnMain(^{
    if (self->_displayLayer != layer) return;
    [self setDisplayLayer:nil];
  });
}

- (BOOL)start {
  if (_running) return YES;

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

  [_session activate];

  _paused = false;
  _interrupted = false;
  _failed = false;
  _clockStarted = false;
  _running = true;
  [_pump startWithAudioDecoder:_audioDecoder videoDecoder:_videoDecoder];
  // The lifecycle observer reports transitions only: a start() issued while
  // already backgrounded must derive the current state itself (main-only API).
  dispatch_async(dispatch_get_main_queue(), ^{
    if (UIApplication.sharedApplication.applicationState ==
        UIApplicationStateBackground) {
      [self suspendForBackground];
    }
  });
  return YES;
}

- (BOOL)stop {
  if (!_running) return YES;
  _running = false;
  [_pump stop];

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
  [_session deactivate];
  // Cleared so the getters do not report a stale pause on a stopped player.
  // _backgrounded deliberately survives: it mirrors app state, not the session.
  _paused = false;
  _interrupted = false;
  return YES;
}

- (BOOL)pause {
  _paused = true;
  if (_synchronizer) [_synchronizer setRate:0];
  [_pump setPaused:YES];
  return YES;
}

- (BOOL)resume {
  _paused = false;
  // Backgrounded or interrupted playback stays stopped until the app returns /
  // the call ends, whatever JS asks for: the decoder has no session and the
  // audio session belongs to the system.
  if (_backgrounded.load() || _interrupted.load()) return YES;
  [_pump setPaused:NO];
  if (_synchronizer && _clockStarted.load()) {
    [_synchronizer setRate:_playbackRate.load()];
  }
  return YES;
}

// Records app state unconditionally (the flag survives stop()/start()), parks
// playback only when running. Apply steps are idempotent for the start probe.
- (void)suspendForBackground {
  _backgrounded.store(true);
  if (!_running.load()) return;
  // Rate 0 rather than leaving it running: the synchronizer's timebase is driven
  // by the host clock and keeps advancing while the process is suspended, so a
  // running rate would return from a 30s background trip 30s ahead of the audio
  // that actually played.
  if (_synchronizer) [_synchronizer setRate:0];
  MEDIA_LOG_I("WebmPlaybackEngine: background at clock %.3fs",
              _synchronizer ? CMTimeGetSeconds(_synchronizer.currentTime) : -1.0);
  [_pump setPaused:YES];
  [_videoDecoder suspend];
}

- (void)resumeFromForeground {
  if (!_backgrounded.exchange(false)) return;
  if (!_running.load()) return;
  // Paired with the background line: if the clock moved across the trip, the
  // suspension did not actually stop the timebase and queued frames are late.
  MEDIA_LOG_I("WebmPlaybackEngine: foreground at clock %.3fs, layer %d",
              _synchronizer ? CMTimeGetSeconds(_synchronizer.currentTime) : -1.0,
              (int)(_displayLayer != nil));
  [_videoDecoder resume];
  // Gated on the JS pause: unparking unconditionally resurrected the measured
  // 401-packet silent hole for pause() → background → foreground — the pump
  // decoded into queues while the renderer stayed stopped.
  if (!_paused.load()) [_pump setPaused:NO];
  // iOS may have deactivated the session while we were away; reactivating is a
  // no-op if it did not.
  [_session activate];
  if (_synchronizer && _clockStarted.load() && !_paused.load()) {
    [_synchronizer setRate:_playbackRate.load()];
  }
}

- (void)interruptionBegan {
  if (!_running.load() || _backgrounded.load()) return;
  _interrupted.store(true);
  // The system already forced the synchronizer's rate to zero. Park the pump so
  // the bytes stay in the ring instead of being decoded into queues nobody
  // drains — the same silent hole the pause() parking exists to prevent.
  [_pump setPaused:YES];
  [self fireHealth:@"interrupted" detail:@"audio session interruption"];
}

- (void)interruptionEnded:(BOOL)shouldResume {
  _interrupted.store(false);
  if (!_running.load() || _backgrounded.load()) return;
  // The interruption deactivated the session; reactivate before restoring rate.
  [_session activate];
  if (!shouldResume || _paused.load()) {
    // System says wait for the user: record the (real) pause; resume() recovers.
    _paused = true;
    return;
  }
  [_pump setPaused:NO];
  if (_synchronizer && _clockStarted.load()) {
    [_synchronizer setRate:_playbackRate.load()];
  }
  [self fireHealth:@"playing" detail:@"interruption ended"];
}

- (BOOL)isRunning {
  return _running.load();
}

- (BOOL)isPaused {
  return _paused.load();
}

- (size_t)feedData:(const uint8_t*)bytes length:(size_t)length {
  if (!_running) return 0;
  return [_pump feedData:bytes length:length];
}

- (BOOL)setEndOfStream {
  [_pump setEndOfStream];
  return YES;
}

- (BOOL)resetStream {
  // A failure belonging to the torn-down stream must not outlive the reset.
  _failed = false;
  [_pump requestReset];
  return YES;
}

- (BOOL)setMuted:(BOOL)muted {
  _muted = muted;
  if (_audioRenderer) _audioRenderer.muted = muted;
  return YES;
}

- (BOOL)setGain:(float)gain {
  gain = std::clamp(gain, kMinGain, kMaxGain);
  _gain = gain;
  if (_audioRenderer) _audioRenderer.volume = gain;
  return YES;
}

- (BOOL)setPlaybackRate:(float)rate {
  rate = std::clamp(rate, kMinPlaybackRate, kMaxPlaybackRate);
  _playbackRate = rate;
  // Backgrounded/interrupted playback holds rate 0 on purpose: restarting the
  // host clock mid-suspension recreates exactly the drift the suspend prevents.
  if (_synchronizer && _running && !_paused && !_backgrounded.load() &&
      !_interrupted.load() && _clockStarted.load()) {
    [_synchronizer setRate:rate];
  }
  return YES;
}

- (WebmPlaybackState)playbackState {
  // Checked before _running so a failure survives stop(), as it does on Android.
  // start() clears it.
  if (_failed.load()) return WebmPlaybackStateFailed;
  if (!_running) return WebmPlaybackStateIdle;
  if (_paused) return WebmPlaybackStatePaused;
  // Platform-halted (background, interruption): PAUSED with isPaused false.
  if (_backgrounded.load() || _interrupted.load()) return WebmPlaybackStatePaused;
  // Read from the decoders, not from mirrored counters: an audio-only stream
  // must leave Buffering too.
  if (_videoDecoder.framesPresented == 0 && _audioDecoder.packetsDecoded == 0) {
    return WebmPlaybackStateBuffering;
  }
  return WebmPlaybackStatePlaying;
}

- (WebmPlaybackMetrics)metrics {
  return WebmCollectMetrics(_pump, _audioDecoder, _videoDecoder, _gain.load(),
                            _muted.load(), _playbackRate.load());
}

- (void)fireHealth:(NSString*)status detail:(NSString*)detail {
  // Transition tracking and the callback both run on main, so _lastHealthStatus
  // needs no lock. Health events are rare enough that the hop is free.
  RunOnMain(^{
    void (^cb)(NSString*, NSString*) = self->_healthCallback;
    if (!cb) return;
    if ([self->_lastHealthStatus isEqualToString:status]) return;
    self->_lastHealthStatus = status;
    cb(status, detail ?: @"");
  });
}

- (void)setHealthCallback:(void (^)(NSString*, NSString*))callback {
  _healthCallback = [callback copy];
}

// Anchors the media timeline to the stream's own timestamps. Without this the
// synchronizer counts from zero at the set rate the moment start() is called,
// so currentTime reports wall-clock since start rather than stream position.
- (void)startClockAtPts:(int64_t)ptsUs {
  if (_clockStarted.exchange(true)) return;
  AVSampleBufferRenderSynchronizer* sync = _synchronizer;
  if (!sync) return;
  CMTime start = CMTimeMake(ptsUs, 1000000);
  __weak WebmPlaybackEngine* weakSelf = self;
  dispatch_async(dispatch_get_main_queue(), ^{
    WebmPlaybackEngine* self_ = weakSelf;
    if (!self_) return;
    // Rate is read here, not captured at the call site: a pause() landing
    // between the two would otherwise be overwritten by a stale nonzero rate.
    // Backgrounded/interrupted hold rate 0 for the same reason as pause.
    float rate = (self_->_paused.load() || self_->_backgrounded.load() ||
                  self_->_interrupted.load())
                     ? 0.0f
                     : self_->_playbackRate.load();
    [sync setRate:rate time:start];
  });
}

#pragma mark - WebmDemuxPumpDelegate

- (void)demuxPump:(id)pump didDecodeFirstSampleAtPts:(int64_t)ptsUs {
  [self startClockAtPts:ptsUs];
}

// Delivered on main after the demux thread has consumed a reset. The new
// stream carries its own timeline: without re-anchoring, its timestamps land
// behind the still-latched clock and every buffer presents as late.
- (void)demuxPumpDidResetStream:(id)pump {
  _clockStarted = false;
  [_audioRenderer flush];
  [_videoDecoder resetForNewStream];
}

- (void)demuxPump:(id)pump
    didReportHealth:(NSString*)status
             detail:(NSString*)detail {
  // Latched here rather than read from the pump, so the state outlives the
  // pump's own per-run flag being cleared by the next start().
  if ([status isEqualToString:@"failed"]) _failed.store(true);
  [self fireHealth:status detail:detail];
}

@end
