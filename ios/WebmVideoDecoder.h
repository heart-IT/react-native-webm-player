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
///
/// Also watches the layer's requiresFlushToResumeDecoding: iOS fails the layer
/// when the app loses decoder access and only a flush makes it accept media
/// again, which the decoder does itself. Call on the main thread.
- (void)setOutputLayer:(nullable AVSampleBufferDisplayLayer*)layer;

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

/// Whether a decode session exists right now. Frames submitted without one
/// (before the first keyframe, or after iOS invalidated it) are dropped.
@property(nonatomic, readonly) BOOL hasSession;

/// Frames actually handed to the display layer. Diverging from the submitted
/// count means frames are decoding but never reaching the screen.
@property(nonatomic, readonly) uint64_t framesPresented;

/// Decoded frames discarded because the pending queue was full — the layer
/// stopped pulling. Renderer-stage drops, the peer of Android's
/// droppedBufferCount.
@property(nonatomic, readonly) uint64_t pendingCapDrops;

/// Decoded frames waiting for the layer to pull them. The demux thread paces
/// on this so the margin stays in the ring, not here.
@property(nonatomic, readonly) size_t pendingFrames;

/// Forwards to the layer watch's performance log. Main thread.
- (void)logLayerPerformance;

/// The media clock in seconds, or negative before it starts. Set before frames
/// flow; read on the decode callback and the render queue.
@property(nonatomic, copy, nullable) double (^clock)(void);

/// Lead time, PTS minus clock, at its smallest since the last call: once at
/// decode completion and once at enqueue onto the layer, plus the running
/// count of frames enqueued already behind the clock. Separates a slow
/// decoder, a slow pull and a layer dropping on its own.
- (NSString*)leadReport;

/// Discard the session, the decoded backlog, and the layer's queue: everything
/// belonging to the previous stream's timeline. For stream reset.
- (void)resetForNewStream;

/// Drain in-flight frames, invalidate the session, release the format desc.
/// Safe to call multiple times.
- (void)shutdown;

@end

NS_ASSUME_NONNULL_END
