// VP9 hardware decoder.
// Wraps VTDecompressionSession with vpcC codec config + iOS 26.2 supplemental
// decoder registration. Decoded CVPixelBuffers are wrapped in CMSampleBuffers
// (with PTS) and enqueued onto the registered AVSampleBufferDisplayLayer; the
// AVSampleBufferRenderSynchronizer drives presentation timing.
#pragma once

#import <AVFoundation/AVFoundation.h>
#include <cstdint>

NS_ASSUME_NONNULL_BEGIN

@interface WebmVideoDecoder : NSObject

- (instancetype)init NS_DESIGNATED_INITIALIZER;

/// Atomic-replace of the output layer. Old frames in flight still render to the
/// previous layer until the VT callback drains; new frames target the new one.
- (void)setOutputLayer:(nullable AVSampleBufferDisplayLayer*)layer;

/// Frame size as declared by the WebM container. Used when a keyframe's VP9
/// uncompressed header cannot be parsed — without it a single unparseable
/// keyframe means the session is never created and no video ever plays.
- (void)setContainerWidth:(int)width height:(int)height;

/// Submit a VP9 packet. Returns NO if the session can't be created or if the
/// frame is rejected (no keyframe seen yet, dimensions unparseable, etc.).
/// Decode is asynchronous; output is enqueued onto the layer in the VT callback.
- (BOOL)submitFrame:(const uint8_t*)data
              length:(size_t)length
              ptsUs:(int64_t)ptsUs
              isKey:(BOOL)isKey;

/// Whether VideoToolbox reports a usable VP9 decoder on this device. When NO,
/// no session can be created and every frame is dropped — the single most
/// useful thing to know when a device shows no picture.
@property(nonatomic, readonly) BOOL hardwareDecodeSupported;

/// Frames actually handed to the display layer. Diverging from the submitted
/// count means frames are decoding but never reaching the screen.
@property(nonatomic, readonly) uint64_t framesPresented;

/// Drain in-flight frames, invalidate the session, release format desc.
/// Safe to call multiple times.
- (void)shutdown;

@end

NS_ASSUME_NONNULL_END
