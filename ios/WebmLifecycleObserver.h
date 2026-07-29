// App background/foreground notifications, as two blocks.
//
// Exists so WebmPlaybackEngine does not have to know about UIKit or manage
// observer registration. Both blocks are invoked on the main thread.
//
// Only the background/foreground pair is observed. Transient interruptions
// (an incoming call, Control Centre) arrive as AVAudioSession interruption
// notifications and are a separate concern.
#pragma once

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface WebmLifecycleObserver : NSObject

/// Blocks are held for the observer's lifetime, so capture the owner weakly.
- (instancetype)initWithOnBackground:(void (^)(void))onBackground
                        onForeground:(void (^)(void))onForeground
    NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

/// Stop observing. Called from dealloc; safe to call earlier and repeatedly.
- (void)invalidate;

@end

NS_ASSUME_NONNULL_END
