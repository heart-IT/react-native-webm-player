#import "WebmPlaybackEngine.h"
#import "WebmAudioDecoder.h"
#import "WebmDemuxPump.h"
#import "WebmVideoDecoder.h"

#import <AudioToolbox/AudioToolbox.h>
#include <atomic>

#include "common/MediaLog.h"

@interface WebmPlaybackEngine () <WebmDemuxPumpDelegate>
@end

@implementation WebmPlaybackEngine {
  WebmDemuxPump* _pump;
  AVSampleBufferDisplayLayer* _displayLayer;
  AVSampleBufferAudioRenderer* _audioRenderer;
  AVSampleBufferRenderSynchronizer* _synchronizer;
  WebmVideoDecoder* _videoDecoder;
  WebmAudioDecoder* _audioDecoder;

  std::atomic<bool> _running;
  std::atomic<bool> _paused;
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
  _failed = false;
  _clockStarted = false;
  _gain = 1.0f;
  _muted = false;
  _playbackRate = 1.0f;
  _lastHealthStatus = @"idle";
  // Outlives start/stop cycles so its cumulative counters do too.
  _pump = [[WebmDemuxPump alloc] init];
  _pump.delegate = self;
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

  _paused = false;
  _failed = false;
  _clockStarted = false;
  _running = true;
  [_pump startWithAudioDecoder:_audioDecoder videoDecoder:_videoDecoder];
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
  return YES;
}

- (BOOL)pause {
  _paused = true;
  if (_synchronizer) [_synchronizer setRate:0];
  return YES;
}

- (BOOL)resume {
  _paused = false;
  if (_synchronizer && _clockStarted.load()) {
    [_synchronizer setRate:_playbackRate.load()];
  }
  return YES;
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
  [_pump requestReset];
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
  if (_synchronizer && _running && !_paused && _clockStarted.load()) {
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
  // Read from the decoders, not from mirrored counters: an audio-only stream
  // must leave Buffering too.
  if (_videoDecoder.framesPresented == 0 && _audioDecoder.packetsDecoded == 0) {
    return WebmPlaybackStateBuffering;
  }
  return WebmPlaybackStatePlaying;
}

- (WebmPlaybackMetrics)metrics {
  WebmPlaybackMetrics m = {0};
  m.bytesFedTotal = _pump.bytesFedTotal;
  // Read from the decoders rather than mirroring into atomics on the demux
  // thread: the renderer keeps draining after the last packet is fed, so a
  // mirror updated only while demuxing freezes mid-playback.
  m.audioPacketsDecoded = _audioDecoder ? _audioDecoder.packetsDecoded : 0;
  // Frames that reached the display layer, not frames handed to VideoToolbox.
  // Android reports renderedOutputBufferCount, so submitting-but-never-showing
  // would otherwise look identical to playing on one platform and not the other.
  m.videoPacketsDecoded = _videoDecoder ? _videoDecoder.framesPresented : 0;
  m.audioUnderruns = _audioDecoder ? _audioDecoder.underruns : 0;
  m.videoFramesDropped = _pump.videoFramesDropped;
  m.audioFramesRecovered = _audioDecoder ? _audioDecoder.framesRecovered : 0;
  m.videoWidth = _pump.videoWidth;
  m.videoHeight = _pump.videoHeight;
  m.gain = _gain.load();
  m.muted = _muted.load();
  m.playbackRate = _playbackRate.load();
  // The synchronizer's clock keeps advancing once started, so after a stream
  // ends it reports wall-clock rather than position. The last presented
  // timestamp is the stream's own timeline, and matches what Android reports.
  int64_t lastPts = _audioDecoder ? _audioDecoder.lastPresentedPtsUs : -1;
  if (lastPts >= 0) m.currentTimeSeconds = static_cast<double>(lastPts) / 1000000.0;
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

// Anchors the media timeline to the stream's own timestamps. Without this the
// synchronizer counts from zero at the set rate the moment start() is called,
// so currentTime reports wall-clock since start rather than stream position.
- (void)startClockAtPts:(int64_t)ptsUs {
  if (_clockStarted.exchange(true)) return;
  AVSampleBufferRenderSynchronizer* sync = _synchronizer;
  if (!sync) return;
  float rate = _paused.load() ? 0.0f : _playbackRate.load();
  CMTime start = CMTimeMake(ptsUs, 1000000);
  dispatch_async(dispatch_get_main_queue(), ^{
    [sync setRate:rate time:start];
  });
}

#pragma mark - WebmDemuxPumpDelegate

- (void)demuxPump:(id)pump didDecodeFirstAudioAtPts:(int64_t)ptsUs {
  [self startClockAtPts:ptsUs];
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
