// Opus decode + PCM presentation for the iOS engine. Peer of WebmVideoDecoder.
//
// Owns the Opus decoder, packet-loss recovery, and the conversion of decoded PCM
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
/// `opusHead` is the track's CodecPrivate; multichannel layouts cannot be
/// decoded without it.
- (BOOL)configureWithSampleRate:(int)sampleRate
                       channels:(int)channels
                       opusHead:(nullable const uint8_t*)opusHead
                   opusHeadSize:(size_t)opusHeadSize;

/// Decode one Opus packet and enqueue it. `durationUs` comes from the container
/// and is used to detect a missing frame ahead of this one, which is recovered
/// from this packet's embedded FEC where possible.
/// Returns NO if the packet could not be decoded or the renderer refused it.
- (BOOL)submitPacket:(const uint8_t*)data
              length:(size_t)length
               ptsUs:(int64_t)ptsUs
          durationUs:(int64_t)durationUs;

/// Drop decoder state so a new stream starts clean.
- (void)reset;

@property(nonatomic, readonly) uint64_t packetsDecoded;
/// Packets the renderer was not ready to accept.
@property(nonatomic, readonly) uint64_t underruns;
/// Frames reconstructed from FEC or concealed by PLC after a detected gap.
@property(nonatomic, readonly) uint64_t framesRecovered;
/// Presentation time of the most recent buffer handed to the renderer, in
/// microseconds. -1 before anything has played.
@property(nonatomic, readonly) int64_t lastPresentedPtsUs;

@end

NS_ASSUME_NONNULL_END
