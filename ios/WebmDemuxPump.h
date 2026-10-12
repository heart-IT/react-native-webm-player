// Turns received WebM bytes into decoded packets.
//
// Owns the byte ring, the WebmDemuxer, and the thread that moves data between
// them: the worklet source writes through feedData(), the thread drains,
// parses, and hands the emitted Opus/VP9 packets to the decoders. Everything
// about *presentation* — the synchronizer, the renderers, the display layer —
// belongs to WebmPlaybackEngine, which owns one of these.
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

/// First sample of the stream decoded — audio when the stream has an audio
/// track, otherwise the first video frame. The media clock starts here rather
/// than at play(), so it never runs ahead of the data.
- (void)demuxPump:(id)pump didDecodeFirstSampleAtPts:(int64_t)ptsUs;

/// A health transition. Delivered on the main queue.
- (void)demuxPump:(id)pump
    didReportHealth:(NSString*)status
             detail:(NSString*)detail;

/// The demux thread consumed a requestReset. Delivered on the main queue.
/// The engine re-anchors the media clock and flushes the renderers here — the
/// new stream carries its own timeline.
- (void)demuxPumpDidResetStream:(id)pump;

@end

@interface WebmDemuxPump : NSObject

- (instancetype)init NS_DESIGNATED_INITIALIZER;

@property(nonatomic, weak) id<WebmDemuxPumpDelegate> delegate;

/// The media clock in seconds, or a negative value before it starts. Read on
/// the demux thread for the summary log: how far decoded audio runs ahead of it.
@property(nonatomic, copy, nullable) double (^clock)(void);

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

/// Accepts muxed WebM bytes from the worklet source's reader thread. Returns
/// the number accepted: all of them, or none when the ring has no room yet.
/// The producer must be stopped before stop() or requestReset().
- (size_t)feedData:(const uint8_t*)bytes length:(size_t)length;

- (void)setEndOfStream;

/// Clears the ring and asks the demux thread to reset its parser. The demuxer
/// lives on that thread, so it cannot be reset directly from here.
- (void)requestReset;

/// Muxed bytes accepted into the ring, cumulative across sessions.
@property(nonatomic, readonly) uint64_t bytesReceived;
@property(nonatomic, readonly) uint64_t videoFramesDropped;
@property(nonatomic, readonly) int videoWidth;
@property(nonatomic, readonly) int videoHeight;

@end

NS_ASSUME_NONNULL_END
