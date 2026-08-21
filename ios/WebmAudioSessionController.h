// Owns the app-wide AVAudioSession for playback: category, activation, and the
// deactivation handshake that lets other apps resume when playback ends.
//
// Split from WebmPlaybackEngine so the engine holds policy (when to activate)
// and this holds the AVAudioSession mechanics (how, and what to tell the rest
// of the system).
#pragma once

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface WebmAudioSessionController : NSObject

/// Playback category, then activate. Safe to call repeatedly: reactivation
/// after an interruption or a background trip is a no-op if still active.
- (void)activate;

/// Deactivate with NotifyOthersOnDeactivation, so audio interrupted by this
/// session gets its resume hint. Call when playback stops.
- (void)deactivate;

@end

NS_ASSUME_NONNULL_END
