// AVAudioSession interruption notifications, as two blocks.
//
// Exists so WebmPlaybackEngine does not have to know about AVAudioSession
// notification plumbing. Both blocks are invoked on the main thread.
//
// The system forces the synchronizer's rate to zero when an interruption
// begins (documented on AVSampleBufferRenderSynchronizer.rate); nothing
// restores it, so without this observer playback stayed silently stopped
// after every phone call.
#pragma once

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface WebmInterruptionObserver : NSObject

/// Blocks are held for the observer's lifetime, so capture the owner weakly.
/// onEnded receives whether the system granted ShouldResume.
- (instancetype)initWithOnBegan:(void (^)(void))onBegan
                        onEnded:(void (^)(BOOL shouldResume))onEnded
    NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

/// Stop observing. Called from dealloc; safe to call earlier and repeatedly.
- (void)invalidate;

@end

NS_ASSUME_NONNULL_END
