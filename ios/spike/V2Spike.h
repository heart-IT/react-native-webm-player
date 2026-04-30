// V2 architecture spike — validates AVSampleBufferRenderSynchronizer + AVSampleBufferAudioRenderer
// + AVSampleBufferDisplayLayer end-to-end with libwebm demux + libopus + VTDecompression.
//
// Entry point: present V2SpikeViewController modally. It loads the bundled bbb fixture,
// drives the full pipeline, and reports completion.
//
// SPIKE — to be deleted at end of Phase 3. Do not depend on these types from production code.
#pragma once

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, V2SpikeResult) {
    V2SpikeResultPlayed,        // Played to completion
    V2SpikeResultDismissed,     // User dismissed before completion
    V2SpikeResultFailed         // Engine failed
};

typedef void (^V2SpikeCompletionHandler)(V2SpikeResult result, NSError* _Nullable error);

@interface V2SpikeViewController : UIViewController

@property (nonatomic, copy, nullable) V2SpikeCompletionHandler completionHandler;

@end

NS_ASSUME_NONNULL_END
