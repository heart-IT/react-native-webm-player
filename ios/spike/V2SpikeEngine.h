// V2 spike playback engine. Single-shot: loads embedded WebM fixture, demuxes,
// decodes audio + video, enqueues into renderers attached to a synchronizer,
// starts playback. Reports completion when both tracks have flushed or on error.
#pragma once

#import <AVFoundation/AVFoundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef void (^V2SpikeEngineCompletion)(NSError* _Nullable error);

@interface V2SpikeEngine : NSObject

- (instancetype)initWithDisplayLayer:(AVSampleBufferDisplayLayer*)displayLayer
                        audioRenderer:(AVSampleBufferAudioRenderer*)audioRenderer
                         synchronizer:(AVSampleBufferRenderSynchronizer*)synchronizer
    NS_DESIGNATED_INITIALIZER;

- (instancetype)init NS_UNAVAILABLE;

- (void)playFixtureWithCompletion:(V2SpikeEngineCompletion)completion;

- (void)stop;

@property (nonatomic, readonly) NSUInteger audioPacketsEnqueued;
@property (nonatomic, readonly) NSUInteger videoPacketsEnqueued;
@property (nonatomic, readonly) Float64 fixtureDurationSeconds;

@end

NS_ASSUME_NONNULL_END
