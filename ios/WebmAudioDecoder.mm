#import "WebmAudioDecoder.h"

#import <AudioToolbox/AudioToolbox.h>

#include <atomic>
#include <vector>

#include "common/MediaLog.h"
#include "playback/OpusDecoderAdapter.h"

namespace {

// Opus tops out at 120ms per packet: 5760 frames at 48 kHz.
constexpr int kMaxOpusFrames = 5760;

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

@implementation WebmAudioDecoder {
  __weak AVSampleBufferAudioRenderer* _renderer;
  media::OpusDecoderAdapter _opus;
  int _sampleRate;
  int _channels;
  std::vector<float> _pcm;

  // Presentation time the next packet is expected at, used to spot a hole.
  int64_t _expectedPtsUs;

  std::atomic<uint64_t> _packetsDecoded;
  std::atomic<uint64_t> _underruns;
  std::atomic<uint64_t> _framesRecovered;
}

- (instancetype)init {
  self = [super init];
  if (!self) return nil;
  _sampleRate = 0;
  _channels = 0;
  _expectedPtsUs = -1;
  _packetsDecoded = 0;
  _underruns = 0;
  _framesRecovered = 0;
  _pcm.resize(static_cast<size_t>(kMaxOpusFrames) * 2);
  return self;
}

- (void)setRenderer:(AVSampleBufferAudioRenderer*)renderer {
  _renderer = renderer;
}

- (BOOL)configureWithSampleRate:(int)sampleRate channels:(int)channels {
  if (sampleRate <= 0 || channels <= 0) return NO;
  if (_opus.isValid() && sampleRate == _sampleRate && channels == _channels) {
    return YES;
  }
  if (!_opus.initialize(sampleRate, channels)) {
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

  // A live producer arrives at real-time pace, so a renderer that is not ready
  // means the session was interrupted rather than that the queue is merely deep.
  // Drop and count instead of blocking the demux thread.
  BOOL accepted = NO;
  AVSampleBufferAudioRenderer* renderer = _renderer;
  if (renderer.isReadyForMoreMediaData) {
    [renderer enqueueSampleBuffer:sb];
    accepted = YES;
  } else {
    _underruns.fetch_add(1);
  }
  CFRelease(sb);
  return accepted;
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
  if (accepted) _packetsDecoded.fetch_add(1);

  _expectedPtsUs =
      ptsUs + (durationUs > 0
                   ? durationUs
                   : static_cast<int64_t>(frames) * 1000000 / _sampleRate);
  return accepted;
}

- (uint64_t)packetsDecoded {
  return _packetsDecoded.load();
}

- (uint64_t)underruns {
  return _underruns.load();
}

- (uint64_t)framesRecovered {
  return _framesRecovered.load();
}

@end
