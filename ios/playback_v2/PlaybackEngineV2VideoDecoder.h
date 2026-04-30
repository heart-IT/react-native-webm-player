// VP9 hardware decoder for the v2 engine.
// Wraps VTDecompressionSession with vpcC codec config + iOS 26.2 supplemental
// decoder registration. Decoded CVPixelBuffers are wrapped in CMSampleBuffers
// (with PTS) and enqueued onto the registered AVSampleBufferDisplayLayer; the
// AVSampleBufferRenderSynchronizer drives presentation timing.
#pragma once

#import <AVFoundation/AVFoundation.h>
#include <cstdint>

NS_ASSUME_NONNULL_BEGIN

@interface PlaybackEngineV2VideoDecoder : NSObject

- (instancetype)init NS_DESIGNATED_INITIALIZER;

/// Atomic-replace of the output layer. Old frames in flight still render to the
/// previous layer until the VT callback drains; new frames target the new one.
- (void)setOutputLayer:(nullable AVSampleBufferDisplayLayer*)layer;

/// Submit a VP9 packet. Returns NO if the session can't be created or if the
/// frame is rejected (no keyframe seen yet, dimensions unparseable, etc.).
/// Decode is asynchronous; output is enqueued onto the layer in the VT callback.
- (BOOL)submitFrame:(const uint8_t*)data
              length:(size_t)length
              ptsUs:(int64_t)ptsUs
              isKey:(BOOL)isKey;

/// Drain in-flight frames, invalidate the session, release format desc.
/// Safe to call multiple times.
- (void)shutdown;

@end

NS_ASSUME_NONNULL_END
