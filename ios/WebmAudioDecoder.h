// Opus decode + PCM presentation for the iOS engine. Peer of WebmVideoDecoder.
//
// Owns the Opus decoder and the conversion of decoded PCM
// into CMSampleBuffers enqueued on an AVSampleBufferAudioRenderer. The engine
// hands it demuxed packets and does not otherwise know how audio is produced.
#pragma once

#import <AVFoundation/AVFoundation.h>
#include <stddef.h>
#include <stdint.h>

NS_ASSUME_NONNULL_BEGIN

@interface WebmAudioDecoder : NSObject

- (instancetype)init NS_DESIGNATED_INITIALIZER;

/// Renderer decoded audio is presented on. Weakly held; the engine owns it.
- (void)setRenderer:(nullable AVSampleBufferAudioRenderer*)renderer;

/// Configure for the container's track parameters. Safe to call repeatedly; the
/// decoder is rebuilt only when they actually change.
- (BOOL)configureWithSampleRate:(int)sampleRate channels:(int)channels;

/// Decode one Opus packet and enqueue it.
/// Returns NO if the packet could not be decoded or the renderer refused it.
- (BOOL)submitPacket:(const uint8_t*)data length:(size_t)length ptsUs:(int64_t)ptsUs;

/// Drop decoder state so a new stream starts clean.
- (void)reset;

@property(nonatomic, readonly) uint64_t packetsDecoded;
/// Presentation time of the most recent buffer handed to the renderer, in
/// microseconds. -1 before anything has played.
@property(nonatomic, readonly) int64_t lastPresentedPtsUs;

/// Decoded buffers waiting for the renderer to pull them. The demux thread
/// paces on this so the margin stays in the ring, not here.
@property(nonatomic, readonly) size_t pendingBuffers;

@end

NS_ASSUME_NONNULL_END
