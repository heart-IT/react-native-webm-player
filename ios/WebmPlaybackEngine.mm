#import "WebmPlaybackEngine.h"
#import "WebmVideoDecoder.h"

#import <AudioToolbox/AudioToolbox.h>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include <opus.h>

#include "common/MediaLog.h"
#include "common/WebMStreamBuffer.h"
#include "demux/WebmDemuxer.h"

namespace {

// Opus tops out at 120ms per packet: 5760 frames at 48 kHz.
constexpr int kMaxOpusFrames = 5760;

// Bytes pulled from the ring per demux pass.
constexpr size_t kPumpChunkBytes = 64 * 1024;

CMSampleBufferRef BuildLPCMSampleBuffer(const float* pcm, int frameCount,
                                        int sampleRate, int channels,
                                        int64_t ptsUs) {
  AudioStreamBasicDescription asbd = {0};
  asbd.mSampleRate = sampleRate;
  asbd.mFormatID = kAudioFormatLinearPCM;
  asbd.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
  asbd.mBytesPerPacket = static_cast<UInt32>(sizeof(float) * channels);
  asbd.mFramesPerPacket = 1;
  asbd.mBytesPerFrame = static_cast<UInt32>(sizeof(float) * channels);
  asbd.mChannelsPerFrame = static_cast<UInt32>(channels);
  asbd.mBitsPerChannel = 32;

  CMAudioFormatDescriptionRef fmt = nullptr;
  if (CMAudioFormatDescriptionCreate(kCFAllocatorDefault, &asbd, 0, nullptr, 0,
                                     nullptr, nullptr, &fmt) != noErr) {
    return nullptr;
  }

  size_t dataSize =
      sizeof(float) * static_cast<size_t>(frameCount) * static_cast<size_t>(channels);
  CMBlockBufferRef block = nullptr;
  if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, dataSize,
                                         kCFAllocatorDefault, nullptr, 0, dataSize,
                                         kCMBlockBufferAssureMemoryNowFlag,
                                         &block) != noErr) {
    CFRelease(fmt);
    return nullptr;
  }
  CMBlockBufferReplaceDataBytes(pcm, block, 0, dataSize);

  CMSampleTimingInfo timing;
  timing.duration = CMTimeMake(frameCount, sampleRate);
  timing.presentationTimeStamp = CMTimeMake(ptsUs, 1000000);
  timing.decodeTimeStamp = kCMTimeInvalid;

  CMSampleBufferRef sb = nullptr;
  OSStatus s = CMSampleBufferCreate(kCFAllocatorDefault, block, true, nullptr,
                                    nullptr, fmt, frameCount, 1, &timing, 0,
                                    nullptr, &sb);
  CFRelease(block);
  CFRelease(fmt);
  return s == noErr ? sb : nullptr;
}

}  // namespace

@implementation WebmPlaybackEngine {
  std::unique_ptr<media::WebMStreamBuffer> _ring;
  AVSampleBufferDisplayLayer* _displayLayer;
  AVSampleBufferAudioRenderer* _audioRenderer;
  AVSampleBufferRenderSynchronizer* _synchronizer;
  WebmVideoDecoder* _videoDecoder;

  OpusDecoder* _opusDecoder;
  int _opusSampleRate;
  int _opusChannels;

  std::thread _demuxThread;
  std::atomic<bool> _running;
  std::atomic<bool> _paused;
  std::atomic<bool> _stop;

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

- (BOOL)start {
  if (_running) return YES;
  _ring = std::make_unique<media::WebMStreamBuffer>(
      media::WebMStreamBuffer::getDefaultCapacity());

  _audioRenderer = [[AVSampleBufferAudioRenderer alloc] init];
  _audioRenderer.muted = _muted.load();
  _audioRenderer.volume = _gain.load();
  _synchronizer = [[AVSampleBufferRenderSynchronizer alloc] init];
  [_synchronizer addRenderer:_audioRenderer];
  if (_displayLayer) [_synchronizer addRenderer:_displayLayer];

  _videoDecoder = [[WebmVideoDecoder alloc] init];
  [_videoDecoder setOutputLayer:_displayLayer];

  [self setupAudioSession];

  _stop = false;
  _paused = false;
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
  if (_opusDecoder) {
    opus_decoder_destroy(_opusDecoder);
    _opusDecoder = nullptr;
  }
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
  if (_ring) _ring->clear();
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

#pragma mark - Opus

// Creates or recreates the Opus decoder for the container's track parameters.
// Guarded against re-entry so a track re-parse cannot leak the previous decoder.
- (BOOL)ensureOpusDecoder:(int)sampleRate channels:(int)channels {
  if (_opusDecoder && sampleRate == _opusSampleRate &&
      channels == _opusChannels) {
    return YES;
  }
  if (_opusDecoder) {
    opus_decoder_destroy(_opusDecoder);
    _opusDecoder = nullptr;
  }
  if (sampleRate <= 0 || channels <= 0) return NO;

  int err = OPUS_OK;
  _opusDecoder = opus_decoder_create(sampleRate, channels, &err);
  if (err != OPUS_OK || !_opusDecoder) {
    _opusDecoder = nullptr;
    MEDIA_LOG_E("WebmPlaybackEngine: opus_decoder_create failed: %d", err);
    return NO;
  }
  _opusSampleRate = sampleRate;
  _opusChannels = channels;
  return YES;
}

#pragma mark - Demux loop

- (void)demuxLoop {
  pthread_setname_np("webmplayer.demux");
  if (!_ring) return;

  media::demux::WebmDemuxer demuxer;
  std::vector<uint8_t> chunk(kPumpChunkBytes);
  std::vector<float> pcm(static_cast<size_t>(kMaxOpusFrames) * 2);
  bool firstAudioSeen = false;
  bool firstVideoSeen = false;

  [self fireHealth:@"buffering" detail:@"waiting for stream data"];

  while (!_stop.load()) {
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
      if ([self ensureOpusDecoder:info.audioSampleRate
                         channels:info.audioChannels]) {
        for (const auto& pkt : result.audioPackets) {
          int frames = opus_decode_float(_opusDecoder, pkt.data,
                                         static_cast<int>(pkt.size), pcm.data(),
                                         kMaxOpusFrames, 0);
          if (frames <= 0) continue;
          CMSampleBufferRef sb =
              BuildLPCMSampleBuffer(pcm.data(), frames, _opusSampleRate,
                                    _opusChannels, pkt.ptsUs);
          if (!sb) continue;
          // A live producer arrives at real-time pace, so a renderer that is
          // not ready means the session was interrupted, not that the queue is
          // merely deep. Drop and count rather than block the demux thread.
          if (_audioRenderer.isReadyForMoreMediaData) {
            [_audioRenderer enqueueSampleBuffer:sb];
            _audioPacketsDecoded.fetch_add(1);
            if (!firstAudioSeen) {
              firstAudioSeen = true;
              [self fireHealth:@"playing" detail:@"first audio frame"];
            }
          } else {
            _audioUnderruns.fetch_add(1);
          }
          CFRelease(sb);
        }
      }
    }

    if (!result.videoPackets.empty()) {
      auto info = demuxer.trackInfoSnapshot();
      _videoWidth.store(info.videoWidth);
      _videoHeight.store(info.videoHeight);
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
  }
}

@end
