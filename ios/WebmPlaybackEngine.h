// iOS playback engine. Owns the WebMStreamBuffer, the demux thread, Opus/VP9
// decode, and the AVSampleBufferRenderSynchronizer. One instance per stream.
//
// Muxed WebM bytes arrive via feedData() (from the Nitro HybridObject on the JS
// thread). A demux thread drains the ring into WebmDemuxer and hands the emitted
// Opus/VP9 packets to the decoders. There is no A/V sync, drift compensation or
// jitter buffer here — the synchronizer owns presentation timing.
// Pure Objective-C interface on purpose: this header is public (it lands in the
// pod's umbrella so Swift can see it), and C++ includes in a modular header trip
// -Wnon-modular-include-in-framework-module. The C++ members live in the .mm.
#pragma once

#import <AVFoundation/AVFoundation.h>
#include <stddef.h>
#include <stdint.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, WebmPlaybackState) {
  WebmPlaybackStateIdle,
  WebmPlaybackStateBuffering,
  WebmPlaybackStatePlaying,
  WebmPlaybackStatePaused,
  WebmPlaybackStateFailed
};

typedef struct {
  uint64_t bytesFedTotal;
  uint64_t audioPacketsDecoded;
  uint64_t videoPacketsDecoded;
  uint64_t audioUnderruns;
  uint64_t videoFramesDropped;
  uint64_t audioFramesRecovered;
  int videoWidth;
  int videoHeight;
  double currentTimeSeconds;
  double playbackRate;
  bool muted;
  float gain;
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

- (BOOL)start;
- (BOOL)stop;
- (BOOL)pause;
- (BOOL)resume;
/// Driven by app lifecycle notifications, not by JS. Stops the media clock and
/// hands back the video decoder for the duration of a background trip.
- (void)suspendForBackground;
- (void)resumeFromForeground;

- (BOOL)isRunning;
- (BOOL)isPaused;

/// Accepts muxed WebM bytes. Returns the number accepted; a short return means
/// the ring is full and the caller is outrunning the demuxer.
- (size_t)feedData:(const uint8_t*)bytes length:(size_t)length;
- (BOOL)setEndOfStream;
- (BOOL)resetStream;

- (BOOL)setMuted:(BOOL)muted;
- (BOOL)setGain:(float)gain;
- (BOOL)setPlaybackRate:(float)rate;

- (WebmPlaybackState)playbackState;
- (WebmPlaybackMetrics)metrics;

- (void)setHealthCallback:(void (^_Nullable)(NSString* status,
                                             NSString* detail))callback;

@end

NS_ASSUME_NONNULL_END
