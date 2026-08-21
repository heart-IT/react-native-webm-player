#import "WebmAudioDecoder.h"

#import <AudioToolbox/AudioToolbox.h>

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

  size_t dataSize = sizeof(float) * static_cast<size_t>(frameCount) *
                    static_cast<size_t>(channels);
  CMBlockBufferRef block = nullptr;
  if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, dataSize,
                                         kCFAllocatorDefault, nullptr, 0, dataSize,
                                         kCMBlockBufferAssureMemoryNowFlag,
                                         &block) != noErr) {
    CFRelease(fmt);
    return nullptr;
  }
  if (CMBlockBufferReplaceDataBytes(pcm, block, 0, dataSize) != noErr) {
    // Building the sample anyway would enqueue uninitialized bytes as audio.
    CFRelease(block);
    CFRelease(fmt);
    return nullptr;
  }

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
  int _sampleRate;
  int _channels;
  std::vector<float> _pcm;

  // Presentation time the next packet is expected at, used to spot a hole.
  int64_t _expectedPtsUs;

  std::atomic<uint64_t> _packetsDecoded;
  std::atomic<uint64_t> _queueOverflowDrops;
  std::atomic<uint64_t> _framesRecovered;
  std::atomic<int64_t> _lastPresentedPtsUs;
}

- (instancetype)init {
  self = [super init];
  if (!self) return nil;
  _sampleRate = 0;
  _channels = 0;
  _expectedPtsUs = -1;
  _packetsDecoded = 0;
  _queueOverflowDrops = 0;
  _framesRecovered = 0;
  _lastPresentedPtsUs = -1;
  _pcm.resize(static_cast<size_t>(kMaxOpusFrames) * 2);
  _feedQueue = dispatch_queue_create("webmplayer.audio.feed", DISPATCH_QUEUE_SERIAL);
  return self;
}

- (void)dealloc {
  if (_requesting) [_renderer stopRequestingMediaData];
  [self drainPending];
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

- (BOOL)configureWithSampleRate:(int)sampleRate
                       channels:(int)channels
                       opusHead:(const uint8_t*)opusHead
                   opusHeadSize:(size_t)opusHeadSize {
  if (sampleRate <= 0 || channels <= 0) return NO;
  if (_opus.isValid() && sampleRate == _sampleRate && channels == _channels) {
    return YES;
  }
  if (!_opus.initialize(sampleRate, channels, opusHead, opusHeadSize)) {
    MEDIA_LOG_E("WebmAudioDecoder: opus init failed (%d Hz, %d ch, err %d)",
                sampleRate, channels, _opus.lastError());
    return NO;
  }
  _sampleRate = sampleRate;
  _channels = channels;
  _pcm.resize(static_cast<size_t>(kMaxOpusFrames) * static_cast<size_t>(channels));
  _expectedPtsUs = -1;
  return YES;
}

- (void)reset {
  [self drainPending];
  // OPUS_RESET_STATE, plus force a reconfigure so the next stream gets a decoder
  // built for its own track parameters rather than the previous stream's.
  _opus.reset();
  _sampleRate = 0;
  _channels = 0;
  _expectedPtsUs = -1;
}

- (BOOL)enqueue:(int)frames atPts:(int64_t)ptsUs {
  if (frames <= 0) return NO;
  CMSampleBufferRef sb =
      BuildLPCMSampleBuffer(_pcm.data(), frames, _sampleRate, _channels, ptsUs);
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
      uint64_t drops = _queueOverflowDrops.fetch_add(1) + 1;
      if ((drops & 511) == 1) {
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

- (BOOL)submitPacket:(const uint8_t*)data
              length:(size_t)length
               ptsUs:(int64_t)ptsUs
          durationUs:(int64_t)durationUs {
  if (!data || length == 0 || !_opus.isValid()) return NO;

  // Exactly one frame missing ahead of this packet: Opus embedded a redundant
  // copy of it in this packet, so recover it before decoding the packet itself.
  // Larger holes are discontinuities (seek, long stall), not loss — concealing
  // those would invent audio rather than restore it.
  if (_expectedPtsUs >= 0 && durationUs > 0) {
    int64_t gap = ptsUs - _expectedPtsUs;
    if (gap >= durationUs && gap < durationUs * 2) {
      int recovered = _opus.decodeFEC(data, length, _pcm.data(), kMaxOpusFrames);
      if (recovered <= 0) {
        recovered = _opus.decodePLC(_pcm.data(), kMaxOpusFrames);
      }
      if (recovered > 0) {
        [self enqueue:recovered atPts:_expectedPtsUs];
        _framesRecovered.fetch_add(1);
      }
    }
  }

  int frames = _opus.decode(data, length, _pcm.data(), kMaxOpusFrames);
  if (frames <= 0) return NO;

  BOOL accepted = [self enqueue:frames atPts:ptsUs];

  _expectedPtsUs =
      ptsUs + (durationUs > 0
                   ? durationUs
                   : static_cast<int64_t>(frames) * 1000000 / _sampleRate);
  return accepted;
}

- (uint64_t)packetsDecoded {
  return _packetsDecoded.load();
}

- (uint64_t)framesRecovered {
  return _framesRecovered.load();
}

- (int64_t)lastPresentedPtsUs {
  return _lastPresentedPtsUs.load();
}

@end
