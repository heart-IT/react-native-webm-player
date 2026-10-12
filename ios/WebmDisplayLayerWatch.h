// Watches an AVSampleBufferDisplayLayer for the two signals iOS gives about a
// layer that stopped accepting media: requiresFlushToResumeDecoding, after
// which only a flush makes it accept media again, and the decode failure with
// its error. A failed layer never invokes the pull block that would otherwise
// notice, so these notifications are the only way a frozen picture is seen.
#pragma once

#import <AVFoundation/AVFoundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface WebmDisplayLayerWatch : NSObject

/// `onFlushNeeded` runs on the main queue whenever the layer reports that it
/// must be flushed to resume decoding. Decode failures are logged here.
- (instancetype)initWithLayer:(AVSampleBufferDisplayLayer*)layer
                onFlushNeeded:(void (^)(void))onFlushNeeded NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

/// Stop observing. Called from dealloc; safe to call earlier and repeatedly.
- (void)invalidate;

/// Logs what the layer's renderer reports about frames it was given: how many
/// it displayed, dropped as late, or found corrupted, and the accumulated
/// delay. Asynchronous; needs iOS 17.4, silent below it.
- (void)logPerformance;

@end

NS_ASSUME_NONNULL_END
