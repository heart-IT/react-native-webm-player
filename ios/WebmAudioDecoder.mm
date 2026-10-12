#import "WebmAudioDecoder.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <vector>

#include "common/MediaLog.h"
#include "playback/OpusDecoderAdapter.h"

namespace {

// Opus tops out at 120ms per packet: 5760 frames at 48 kHz.
constexpr int kMaxOpusFrames = 5760;

// ~2s of 20ms packets. Deep enough to absorb burst arrival from a P2P feed,
// shallow enough that a stalled renderer does not accumulate unbounded latency.
constexpr size_t kMaxPendingBuffers = 100;

// Per-packet logs sample every 512th occurrence — these paths run at packet
// rate (~50/s) when stuck.
constexpr uint64_t kDropLogMask = 511;

// One per configuration, not per packet: the track fixes the format.
CMAudioFormatDescriptionRef CreateLPCMFormat(int sampleRate, int channels) {
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
  return fmt;
}

CMSampleBufferRef BuildLPCMSampleBuffer(const float* pcm, int frameCount,
                                        CMAudioFormatDescriptionRef fmt,
                                        int sampleRate, int channels,
                                        int64_t ptsUs) {
  size_t dataSize = sizeof(float) * static_cast<size_t>(frameCount) *
                    static_cast<size_t>(channels);
  CMBlockBufferRef block = nullptr;
  if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, dataSize,
                                         kCFAllocatorDefault, nullptr, 0, dataSize,
                                         kCMBlockBufferAssureMemoryNowFlag,
                                         &block) != noErr) {
    return nullptr;
  }
  if (CMBlockBufferReplaceDataBytes(pcm, block, 0, dataSize) != noErr) {
    // Building the sample anyway would enqueue uninitialized bytes as audio.
    CFRelease(block);
    return nullptr;
  }

  CMSampleTimingInfo timing;
  // With a single timing entry covering all samples, `duration` is the duration
  // of ONE sample (CMSampleBufferCreate contract) — one PCM frame, not the
  // whole packet.
  timing.duration = CMTimeMake(1, sampleRate);
  timing.presentationTimeStamp = CMTimeMake(ptsUs, 1000000);
  timing.decodeTimeStamp = kCMTimeInvalid;

  CMSampleBufferRef sb = nullptr;
  OSStatus s = CMSampleBufferCreate(kCFAllocatorDefault, block, true, nullptr,
                                    nullptr, fmt, frameCount, 1, &timing, 0,
                                    nullptr, &sb);
  CFRelease(block);
  return s == noErr ? sb : nullptr;
}

}  // namespace

@implementation WebmAudioDecoder {
  __weak AVSampleBufferAudioRenderer* _renderer;
  // Decoded buffers waiting for the renderer to pull them.
  std::deque<CMSampleBufferRef> _pending;
  std::mutex _pendingMutex;
  dispatch_queue_t _feedQueue;
  // Whether a media-data request is currently outstanding. Touched only on
  // _feedQueue, so it needs no lock.
  BOOL _requesting;
  media::OpusDecoderAdapter _opus;
  CMAudioFormatDescriptionRef _format;
  int _sampleRate;
  int _channels;
  std::vector<float> _pcm;

  // Atomics only where a counter crosses threads (written on _feedQueue or the
  // demux thread, read by the JS-thread metrics getters). The log-sampling
  // counters below them live and die on the demux thread.
  std::atomic<uint64_t> _packetsDecoded;
  std::atomic<int64_t> _lastPresentedPtsUs;
  uint64_t _queueOverflowDrops;
  uint64_t _decodeErrors;
  uint64_t _configFailures;
}

- (instancetype)init {
  self = [super init];
  if (!self) return nil;
  _format = nullptr;
  _sampleRate = 0;
  _channels = 0;
  _packetsDecoded = 0;
  _queueOverflowDrops = 0;
  _decodeErrors = 0;
  _configFailures = 0;
  _lastPresentedPtsUs = -1;
  _pcm.resize(static_cast<size_t>(kMaxOpusFrames) * 2);
  _feedQueue = dispatch_queue_create("webmplayer.audio.feed", DISPATCH_QUEUE_SERIAL);
  return self;
}

- (void)dealloc {
  if (_requesting) [_renderer stopRequestingMediaData];
  [self drainPending];
  if (_format) CFRelease(_format);
}

- (void)setRenderer:(AVSampleBufferAudioRenderer*)renderer {
  // Every request must be cancelled before the renderer is released; the header
  // states that releasing without it is undefined behaviour.
  if (_requesting) {
    [_renderer stopRequestingMediaData];
    _requesting = NO;
  }
  _renderer = renderer;
  if (!renderer) return;
  dispatch_async(_feedQueue, ^{
    [self armRequestFor:renderer];
  });
}

// Pull, not push. `isReadyForMoreMediaData` is only meaningful inside this
// callback loop; polling it from the demux thread reported NO almost always and
// silently discarded nearly every packet.
- (void)armRequestFor:(AVSampleBufferAudioRenderer*)renderer {
  if (_requesting) return;
  _requesting = YES;
  __weak WebmAudioDecoder* weakSelf = self;
  [renderer requestMediaDataWhenReadyOnQueue:_feedQueue
                                  usingBlock:^{
    WebmAudioDecoder* strongSelf = weakSelf;
    if (!strongSelf) return;
    [strongSelf feedRenderer:renderer];
  }];
}

- (void)feedRenderer:(AVSampleBufferAudioRenderer*)renderer {
  while (renderer.isReadyForMoreMediaData) {
    CMSampleBufferRef next = nullptr;
    {
      std::lock_guard<std::mutex> lock(_pendingMutex);
      if (_pending.empty()) {
        // Out of data while the renderer still wants more. Returning here would
        // end the requests silently — the renderer only re-invokes the block
        // when it *becomes* ready, which it already is. Cancel explicitly and
        // re-arm when the next packet arrives.
        _requesting = NO;
        [renderer stopRequestingMediaData];
        return;
      }
      next = _pending.front();
      _pending.pop_front();
    }
    CMTime pts = CMSampleBufferGetPresentationTimeStamp(next);
    [renderer enqueueSampleBuffer:next];
    CFRelease(next);
    _packetsDecoded.fetch_add(1);
    if (CMTIME_IS_VALID(pts)) {
      _lastPresentedPtsUs.store(
          CMTimeGetSeconds(pts) * 1000000, std::memory_order_relaxed);
    }
  }
}

- (void)drainPending {
  std::lock_guard<std::mutex> lock(_pendingMutex);
  for (CMSampleBufferRef sb : _pending) CFRelease(sb);
  _pending.clear();
}

- (BOOL)configureWithSampleRate:(int)sampleRate channels:(int)channels {
  if (_opus.isValid() && sampleRate == _sampleRate && channels == _channels) {
    return YES;
  }
  if (!_opus.initialize(sampleRate, channels)) {
    // Retried every packet batch while the failure persists; sample the log.
    uint64_t fails = ++_configFailures;
    if ((fails & kDropLogMask) == 1) {
      MEDIA_LOG_E("WebmAudioDecoder: opus init failed (%d Hz, %d ch, err %d)",
                  sampleRate, channels, _opus.lastError());
    }
    return NO;
  }
  CMAudioFormatDescriptionRef format = CreateLPCMFormat(sampleRate, channels);
  if (!format) return NO;
  if (_format) CFRelease(_format);
  _format = format;
  _sampleRate = sampleRate;
  _channels = channels;
  _pcm.resize(static_cast<size_t>(kMaxOpusFrames) * static_cast<size_t>(channels));
  return YES;
}

- (void)reset {
  [self drainPending];
  // OPUS_RESET_STATE, plus force a reconfigure so the next stream gets a decoder
  // built for its own track parameters rather than the previous stream's.
  _opus.reset();
  if (_format) {
    CFRelease(_format);
    _format = nullptr;
  }
  _sampleRate = 0;
  _channels = 0;
}

- (BOOL)enqueue:(int)frames atPts:(int64_t)ptsUs {
  if (frames <= 0 || !_format) return NO;
  CMSampleBufferRef sb = BuildLPCMSampleBuffer(_pcm.data(), frames, _format,
                                               _sampleRate, _channels, ptsUs);
  if (!sb) return NO;

  {
    std::lock_guard<std::mutex> lock(_pendingMutex);
    // Drop the newest rather than the oldest: the queue is ordered by
    // presentation time, and discarding from the front would tear a hole in
    // audio the renderer is about to play.
    if (_pending.size() >= kMaxPendingBuffers) {
      // An overflow (renderer not pulling), not an underrun — it was counted
      // as one for a while, which made the same metrics field mean "too much
      // data" here and "too little" on Android. Rate-limited: a stalled
      // renderer hits this fifty times a second.
      uint64_t drops = ++_queueOverflowDrops;
      if ((drops & kDropLogMask) == 1) {
        MEDIA_LOG_W("WebmAudioDecoder: pending queue full, %llu packets dropped",
                    static_cast<unsigned long long>(drops));
      }
      CFRelease(sb);
      return NO;
    }
    _pending.push_back(sb);  // ownership moves to the queue
  }

  AVSampleBufferAudioRenderer* renderer = _renderer;
  if (renderer) {
    dispatch_async(_feedQueue, ^{
      [self armRequestFor:renderer];
    });
  }
  return YES;
}

- (BOOL)submitPacket:(const uint8_t*)data length:(size_t)length ptsUs:(int64_t)ptsUs {
  if (!data || length == 0 || !_opus.isValid()) return NO;

  int frames = _opus.decode(data, length, _pcm.data(), kMaxOpusFrames);
  if (frames <= 0) {
    // Error table: log, drop, continue — with a count, not silently.
    uint64_t errs = ++_decodeErrors;
    if ((errs & kDropLogMask) == 1) {
      MEDIA_LOG_W("WebmAudioDecoder: opus decode failed (err %d, %llu total)",
                  _opus.lastError(), static_cast<unsigned long long>(errs));
    }
    return NO;
  }

  return [self enqueue:frames atPts:ptsUs];
}

- (size_t)pendingBuffers {
  std::lock_guard<std::mutex> lock(_pendingMutex);
  return _pending.size();
}

- (uint64_t)packetsDecoded {
  return _packetsDecoded.load();
}

- (int64_t)lastPresentedPtsUs {
  return _lastPresentedPtsUs.load();
}

@end
