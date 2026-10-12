#import "WebmPlaybackEngine.h"
#import "WebmAudioDecoder.h"
#import "WebmAudioSessionController.h"
#import "WebmDemuxPump.h"
#import "WebmEngineMetrics.h"
#import "WebmInterruptionObserver.h"
#import "WebmPlaybackPolicy.h"
#import "WebmVideoDecoder.h"
#import "WebmWorkletSource.h"

#include <atomic>
#include <mutex>

#include "common/MediaLog.h"

namespace {

void RunOnMain(void (^block)(void)) {
  if ([NSThread isMainThread]) {
    block();
  } else {
    dispatch_async(dispatch_get_main_queue(), block);
  }
}

}  // namespace

@interface WebmPlaybackEngine () <WebmDemuxPumpDelegate, WebmWorkletSourceDelegate>
@end

@implementation WebmPlaybackEngine {
  WebmDemuxPump* _pump;
  WebmAudioSessionController* _session;
  WebmInterruptionObserver* _interruption;
  AVSampleBufferDisplayLayer* _displayLayer;
  AVSampleBufferAudioRenderer* _audioRenderer;
  AVSampleBufferRenderSynchronizer* _synchronizer;
  WebmVideoDecoder* _videoDecoder;
  WebmAudioDecoder* _audioDecoder;

  // Taken by everything that starts, stops or re-wires a session: play/stop on the
  // JS thread, status, interruptions, the display layer and the rejoin timer on main.
  std::recursive_mutex _control;
  WebmWorkletSource* _source;
  NSString* _key;
  int _datalessSessions;
  dispatch_block_t _pendingRejoin;
  // The worklet said `ended`: its exit is the broadcast finishing, not a loss.
  bool _streamEnded;

  std::atomic<bool> _running;
  // Latched from the pump's terminal-parse-error health event and from a
  // failed rejoin, so a failure survives stop() as it does on Android.
  std::atomic<bool> _failed;
  // The synchronizer's clock is the media timeline, started at the first
  // sample's presentation time: started earlier it runs ahead of the stream.
  std::atomic<bool> _clockStarted;
  std::atomic<bool> _muted;
  // Set on the source's reader thread at the first byte of a session.
  std::atomic<bool> _sessionReceivedData;

  void (^_healthCallback)(NSString*, NSString*);
  NSString* _lastHealthStatus;  // Main-thread only.
  NSString* _lastHealthDetail;  // Main-thread only.
}

- (instancetype)init {
  self = [super init];
  if (!self) return nil;
  _running = false;
  _failed = false;
  _clockStarted = false;
  _muted = false;
  _sessionReceivedData = false;
  _datalessSessions = 0;
  _streamEnded = false;
  _lastHealthStatus = @"idle";
  _lastHealthDetail = @"";
  _session = [[WebmAudioSessionController alloc] init];
  // Outlives play/stop cycles so its cumulative counters do too.
  _pump = [[WebmDemuxPump alloc] init];
  _pump.delegate = self;

  __weak WebmPlaybackEngine* weakSelf = self;
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
  // CALayer parenting and the synchronizer's renderer set are main-thread only;
  // play() builds that set on the JS thread, so this takes _control as well.
  RunOnMain(^{
    std::lock_guard<std::recursive_mutex> lock(self->_control);
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

#pragma mark - Sessions

- (void)playWithKey:(NSString*)key {
  std::lock_guard<std::recursive_mutex> lock(_control);
  [self stop];
  _key = [key copy];

  _audioRenderer = [[AVSampleBufferAudioRenderer alloc] init];
  _audioRenderer.muted = _muted.load();
  _synchronizer = [[AVSampleBufferRenderSynchronizer alloc] init];
  [_synchronizer addRenderer:_audioRenderer];
  if (_displayLayer) [_synchronizer addRenderer:_displayLayer];

  _videoDecoder = [[WebmVideoDecoder alloc] init];
  [_videoDecoder setOutputLayer:_displayLayer];
  _audioDecoder = [[WebmAudioDecoder alloc] init];
  [_audioDecoder setRenderer:_audioRenderer];

  [_session activate];

  _failed = false;
  _clockStarted = false;
  _streamEnded = false;
  _datalessSessions = 0;
  _sessionReceivedData = false;
  _running = true;
  AVSampleBufferRenderSynchronizer* sync = _synchronizer;
  _videoDecoder.clock = _pump.clock = ^double { return CMTimeGetSeconds(sync.currentTime); };
  [_pump startWithAudioDecoder:_audioDecoder videoDecoder:_videoDecoder];
  [self startSource];
}

- (void)stop {
  std::lock_guard<std::recursive_mutex> lock(_control);
  if (!_running) return;
  _running = false;
  [self cancelPendingRejoin];
  // The source is joined before the pump it writes into goes away.
  [_source stop];
  _source = nil;
  [_pump stop];
  _videoDecoder.clock = _pump.clock = nil;

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
}

// Starts a worklet for the current key, feeding the pump. _control held.
- (void)startSource {
  WebmWorkletSource* source = [[WebmWorkletSource alloc] init];
  source.delegate = self;
  WebmDemuxPump* pump = _pump;
  // A raw pointer, not a weak self: stop() joins the reader before the engine
  // can go away, and a weak load there could take the last strong reference.
  std::atomic<bool>* receivedData = &_sessionReceivedData;
  _sessionReceivedData = false;
  BOOL started = [source startWithKey:_key
                                 sink:^size_t(const uint8_t* bytes, size_t length) {
                                   size_t wrote = [pump feedData:bytes length:length];
                                   if (wrote) receivedData->store(true);
                                   return wrote;
                                 }];
  if (!started) {
    _failed = true;
    [self fireHealth:@"failed" detail:@"feeder bundle missing from the app"];
    [self stop];
    return;
  }
  _source = source;
}

// Drops the current source and starts another after a backoff. _control held.
- (void)rejoinWithReason:(NSString*)reason {
  [_source stop];
  _source = nil;
  [self cancelPendingRejoin];

  if (_sessionReceivedData.load()) {
    _datalessSessions = 0;
  } else if (++_datalessSessions >= kMaxDatalessSessions) {
    _failed = true;
    [self fireHealth:@"failed"
              detail:[NSString stringWithFormat:@"rejoin failed: %@", reason]];
    [self stop];
    return;
  }
  double delay = WebmRejoinDelaySeconds(_datalessSessions);
  MEDIA_LOG_I("WebmPlaybackEngine: rejoining in %.1fs (%s)", delay, reason.UTF8String);
  [self fireHealth:@"buffering"
            detail:[NSString stringWithFormat:@"rejoining: %@", reason]];

  __weak WebmPlaybackEngine* weakSelf = self;
  __block dispatch_block_t block = dispatch_block_create(DISPATCH_BLOCK_INHERIT_QOS_CLASS, ^{
    WebmPlaybackEngine* self_ = weakSelf;
    if (!self_) return;
    std::lock_guard<std::recursive_mutex> lock(self_->_control);
    if (!self_->_running || self_->_pendingRejoin != block) return;
    self_->_pendingRejoin = nil;
    // The old source is joined, so nothing writes while the ring is cleared;
    // the demux thread resets its parser and demuxPumpDidResetStream re-anchors.
    [self_->_pump requestReset];
    [self_ startSource];
  });
  _pendingRejoin = block;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), block);
}

- (void)cancelPendingRejoin {
  if (!_pendingRejoin) return;
  dispatch_block_cancel(_pendingRejoin);
  _pendingRejoin = nil;
}

- (void)interruptionBegan {
  std::lock_guard<std::recursive_mutex> lock(_control);
  if (!_running || !_source) return;
  // The system already forced the synchronizer's rate to zero. Stop feeding
  // too: what the worklet produces during the call is behind the live edge.
  [self cancelPendingRejoin];
  [_source stop];
  _source = nil;
  [self fireHealth:@"buffering" detail:@"audio interrupted"];
}

- (void)interruptionEnded:(BOOL)shouldResume {
  std::lock_guard<std::recursive_mutex> lock(_control);
  if (!_running || _source) return;
  [_session activate];  // the interruption deactivated it
  // Without ShouldResume the system wants the user to restart playback.
  if (!shouldResume) return;
  [self rejoinWithReason:@"interruption ended"];
}

#pragma mark - Controls and state

// Getters take _control too: stop() nils the decoders and renderer under it while JS polls.
- (void)setMuted:(BOOL)muted {
  std::lock_guard<std::recursive_mutex> lock(_control);
  _muted = muted;
  if (_audioRenderer) _audioRenderer.muted = muted;
}

- (BOOL)muted { return _muted.load(); }

- (WebmPlaybackState)playbackState {
  std::lock_guard<std::recursive_mutex> lock(_control);
  // Checked before _running so a failure survives stop(), as on Android; play() clears it.
  if (_failed.load()) return WebmPlaybackStateFailed;
  if (!_running) return WebmPlaybackStateIdle;
  // From the decoders, not mirrored counters: an audio-only stream must leave Buffering too.
  if (_videoDecoder.framesPresented == 0 && _audioDecoder.packetsDecoded == 0) {
    return WebmPlaybackStateBuffering;
  }
  return WebmPlaybackStatePlaying;
}

- (WebmPlaybackMetrics)metrics {
  std::lock_guard<std::recursive_mutex> lock(_control);
  return WebmCollectMetrics(_pump, _audioDecoder, _videoDecoder);
}

- (void)fireHealth:(NSString*)status detail:(NSString*)detail {
  // Transition tracking and the callback both run on main, so the last-event
  // fields need no lock. Health events are rare enough that the hop is free.
  RunOnMain(^{
    void (^cb)(NSString*, NSString*) = self->_healthCallback;
    if (!cb) return;
    NSString* d = detail ?: @"";
    if ([self->_lastHealthStatus isEqualToString:status] &&
        [self->_lastHealthDetail isEqualToString:d]) {
      return;
    }
    self->_lastHealthStatus = status;
    self->_lastHealthDetail = d;
    // The one trace of the session that survives into a collected device log.
    MEDIA_LOG_I("WebmPlaybackEngine: health %s (%s)", status.UTF8String, d.UTF8String);
    cb(status, d);
  });
}

- (void)setHealthCallback:(void (^)(NSString*, NSString*))callback {
  _healthCallback = [callback copy];
}

// Anchors the media timeline to the stream's own timestamps, kLiveDelaySec
// behind the first sample: the renderers hold what is enqueued until its time
// comes, so the margin lives in the platform clock, not in a buffer of ours.
- (void)startClockAtPts:(int64_t)ptsUs {
  if (_clockStarted.exchange(true)) return;
  AVSampleBufferRenderSynchronizer* sync = _synchronizer;
  if (!sync) return;
  int64_t startUs = ptsUs - static_cast<int64_t>(kLiveDelaySec * 1000000);
  CMTime start = CMTimeMake(startUs > 0 ? startUs : 0, 1000000);
  dispatch_async(dispatch_get_main_queue(), ^{
    [sync setRate:1.0f time:start];
  });
}

#pragma mark - WebmWorkletSourceDelegate (main queue)

- (void)workletSource:(id)source didReportStatus:(NSString*)status {
  std::lock_guard<std::recursive_mutex> lock(_control);
  if (source != _source || !_running) return;
  if ([status hasPrefix:@"net "]) {  // the swarm as the worklet sees it: log only
    MEDIA_LOG_I("WebmPlaybackEngine: worklet %s", status.UTF8String);
    return;
  }
  if ([status isEqualToString:@"joining"] || [status hasPrefix:@"connected"]) {
    [self fireHealth:@"buffering"
              detail:[NSString stringWithFormat:@"%@, waiting for data", status]];
    return;
  }
  if ([status isEqualToString:@"ended"]) {
    _streamEnded = true;
    // The pump drains what is buffered, reports `ended`, and the engine stops itself there.
    [_pump setEndOfStream];
    return;
  }
  // `lag`, `stalled`, `reconfigured`, `error <message>` or unknown: the stream is lost.
  [self rejoinWithReason:status];
}

- (void)workletSourceDidEnd:(id)source {
  std::lock_guard<std::recursive_mutex> lock(_control);
  if (source != _source || !_running || _streamEnded) return;
  [self rejoinWithReason:@"worklet exited"];
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
  if ([status isEqualToString:@"failed"]) {
    // A terminal parse error is a lost stream, not a dead player: the rejoin
    // resets the parser and the dataless-session limit bounds retries.
    std::lock_guard<std::recursive_mutex> lock(_control);
    if (_running) [self rejoinWithReason:detail];
    return;
  }
  [self fireHealth:status detail:detail];
  // A finished broadcast leaves the player stopped; play() starts another.
  if ([status isEqualToString:@"ended"]) [self stop];
}

@end
