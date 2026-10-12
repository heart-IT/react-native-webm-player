// iOS playback engine. Owns the broadcast worklet source, the demux pump, the
// Opus/VP9 decoders, and the AVSampleBufferRenderSynchronizer. One instance per
// player.
//
// play(key) starts a worklet that joins the broadcast and streams muxed WebM
// over its IPC; the source writes those bytes into the pump's ring, the demux
// thread decodes them, and the synchronizer owns presentation timing. There is
// no A/V sync, drift compensation or jitter buffer here. Any loss of the
// stream — the worklet falling behind the live edge, an error, an audio
// interruption ending — is handled by rejoining at the live edge.
//
// Pure Objective-C interface on purpose: this header is public (it lands in the
// pod's umbrella so Swift can see it), and C++ includes in a modular header trip
// -Wnon-modular-include-in-framework-module. The C++ members live in the .mm.
#pragma once

#import <AVFoundation/AVFoundation.h>
#include <stdint.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, WebmPlaybackState) {
  WebmPlaybackStateIdle,
  WebmPlaybackStateBuffering,
  WebmPlaybackStatePlaying,
  WebmPlaybackStateFailed
};

typedef struct {
  uint64_t bytesReceived;
  uint64_t audioPacketsDecoded;
  uint64_t videoPacketsDecoded;
  uint64_t videoFramesDropped;
  int videoWidth;
  int videoHeight;
  double currentTimeSeconds;
} WebmPlaybackMetrics;

@interface WebmPlaybackEngine : NSObject

- (instancetype)init NS_DESIGNATED_INITIALIZER;

/// Attach the layer the decoded video is presented on. Safe from any thread.
- (void)setDisplayLayer:(nullable AVSampleBufferDisplayLayer*)layer;

/// Clear the layer, but only if it is still the attached one. A view torn down
/// after a newer one attached must not detach the newer surface.
///
/// NS_SWIFT_NAME because the importer otherwise strips the redundant type name
/// and exposes this as `detach(_:)`, which reads as if it detached the engine.
- (void)detachDisplayLayer:(AVSampleBufferDisplayLayer*)layer
    NS_SWIFT_NAME(detachDisplayLayer(_:));

/// Join the broadcast at `key` and play from the live edge. Stops any current
/// playback first.
- (void)playWithKey:(NSString*)key;
- (void)stop;

- (void)setMuted:(BOOL)muted;
- (BOOL)muted;

- (WebmPlaybackState)playbackState;
- (WebmPlaybackMetrics)metrics;

- (void)setHealthCallback:(void (^_Nullable)(NSString* status,
                                             NSString* detail))callback;

@end

NS_ASSUME_NONNULL_END
