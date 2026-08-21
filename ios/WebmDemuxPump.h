// Turns fed WebM bytes into decoded packets.
//
// Owns the byte ring, the WebmDemuxer, and the thread that moves data between
// them: feedData() writes, the thread drains, parses, and hands the emitted
// Opus/VP9 packets to the decoders. Everything about *presentation* — the
// synchronizer, the renderers, the display layer — belongs to
// WebmPlaybackEngine, which owns one of these.
//
// Pure Objective-C interface, matching WebmPlaybackEngine's rationale: the C++
// ring and demuxer are implementation details that stay in the .mm.
#pragma once

#import <Foundation/Foundation.h>
#include <stddef.h>
#include <stdint.h>

@class WebmAudioDecoder;
@class WebmVideoDecoder;

NS_ASSUME_NONNULL_BEGIN

@protocol WebmDemuxPumpDelegate <NSObject>

/// First audio packet of the stream decoded. The media clock starts here rather
/// than at start(), so it never runs ahead of the data.
- (void)demuxPump:(id)pump didDecodeFirstAudioAtPts:(int64_t)ptsUs;

/// A health transition. Delivered on the main queue.
- (void)demuxPump:(id)pump
    didReportHealth:(NSString*)status
             detail:(NSString*)detail;

@end

@interface WebmDemuxPump : NSObject

- (instancetype)init NS_DESIGNATED_INITIALIZER;

@property(nonatomic, weak) id<WebmDemuxPumpDelegate> delegate;

/// Allocates the ring and starts the demux thread.
///
/// The decoders belong to the engine, which wires them to the renderer and
/// display layer and reads their counters for metrics; the pump only drives
/// them, and releases them again on stop(). The pump itself outlives a
/// start/stop cycle so its cumulative counters do too.
- (void)startWithAudioDecoder:(WebmAudioDecoder*)audioDecoder
                 videoDecoder:(WebmVideoDecoder*)videoDecoder;

/// Shuts the ring down and joins the thread. Idempotent.
- (void)stop;

/// Park the demux thread without tearing anything down.
///
/// Stopping the renderer alone is not enough: the thread would keep draining the
/// ring and decoding into a queue nobody consumes, and the decoder discards what
/// does not fit — silently losing audio for the whole pause. Parked, the bytes
/// stay in the ring and playback continues from exactly where it stopped.
- (void)setPaused:(BOOL)paused;

/// Accepts muxed WebM bytes. Returns the number accepted; short means the ring
/// is full and the caller is outrunning the demuxer.
- (size_t)feedData:(const uint8_t*)bytes length:(size_t)length;

- (void)setEndOfStream;

/// Clears the ring and asks the demux thread to reset its parser. The demuxer
/// lives on that thread, so it cannot be reset directly from here.
- (void)requestReset;

@property(nonatomic, readonly) uint64_t bytesFedTotal;
@property(nonatomic, readonly) uint64_t videoFramesDropped;
@property(nonatomic, readonly) int videoWidth;
@property(nonatomic, readonly) int videoHeight;

@end

NS_ASSUME_NONNULL_END
