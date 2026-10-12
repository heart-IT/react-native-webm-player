// Runs the broadcast worklet for one key and pumps its IPC into a byte sink.
//
// The worklet (worklet/feeder.js, shipped as the feeder.bundle resource) joins
// the swarm and writes length-prefixed frames: muxed WebM bytes, or a status
// string. This class owns the worklet, the IPC and the thread that reads it;
// it knows nothing about demuxing or presentation. One instance per join.
#pragma once

#import <Foundation/Foundation.h>
#include <stddef.h>
#include <stdint.h>

NS_ASSUME_NONNULL_BEGIN

@protocol WebmWorkletSourceDelegate <NSObject>

/// A status frame: `connected`, `lag`, `ended`, or `error <message>`.
/// Delivered on the main queue.
- (void)workletSource:(id)source didReportStatus:(NSString*)status;

/// The IPC reached end of stream: the worklet exited. Delivered on the main
/// queue, and never after stop().
- (void)workletSourceDidEnd:(id)source;

@end

/// Accepts a run of muxed WebM bytes. Returns the count accepted; a short
/// return means no room yet, and the same bytes are offered again.
typedef size_t (^WebmByteSink)(const uint8_t* bytes, size_t length);

@interface WebmWorkletSource : NSObject

- (instancetype)init NS_DESIGNATED_INITIALIZER;

@property(nonatomic, weak) id<WebmWorkletSourceDelegate> delegate;

/// Starts the worklet for `key` and begins pumping. Returns NO when the
/// feeder bundle resource is missing, which is a packaging error.
- (BOOL)startWithKey:(NSString*)key sink:(WebmByteSink)sink;

/// Terminates the worklet and joins the reader. Idempotent.
- (void)stop;

/// Muxed bytes handed to the sink so far.
@property(nonatomic, readonly) uint64_t bytesReceived;

@end

NS_ASSUME_NONNULL_END
