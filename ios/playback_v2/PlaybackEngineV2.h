// V2 iOS playback engine. Owns the WebMStreamBuffer, demux thread, audio + video
// pumps, and AVSampleBufferRenderSynchronizer. Single instance per stream session.
//
// The engine accepts muxed WebM bytes via feedData() (called from JSI), demuxes
// them off-thread, decodes Opus + VP9, and presents via Apple's renderer/synchronizer
// pair. No A/V sync code, drift compensation, or jitter buffer — those are owned
// by AVFoundation.
#pragma once

#import <AVFoundation/AVFoundation.h>
#include <atomic>
#include <cstdint>
#include <memory>

@class PlaybackEngineV2Internal;

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, PlaybackEngineState) {
    PlaybackEngineStateIdle,
    PlaybackEngineStateBuffering,
    PlaybackEngineStatePlaying,
    PlaybackEngineStatePaused,
    PlaybackEngineStateFailed
};

typedef struct {
    uint64_t bytesFedTotal;
    uint64_t audioPacketsDecoded;
    uint64_t videoPacketsDecoded;
    uint64_t audioUnderruns;
    uint64_t videoFramesDropped;
    int videoWidth;
    int videoHeight;
    double currentTimeSeconds;
    double playbackRate;
    bool muted;
    float gain;
} PlaybackEngineV2Metrics;

@interface PlaybackEngineV2 : NSObject

- (instancetype)init NS_DESIGNATED_INITIALIZER;

- (void)setDisplayLayer:(nullable AVSampleBufferDisplayLayer*)layer;

- (BOOL)start;
- (BOOL)stop;
- (BOOL)pause;
- (BOOL)resume;
- (BOOL)isRunning;
- (BOOL)isPaused;

- (size_t)feedData:(const uint8_t*)bytes length:(size_t)length;
- (BOOL)setEndOfStream;
- (BOOL)resetStream;

- (BOOL)setMuted:(BOOL)muted;
- (BOOL)setGain:(float)gain;
- (BOOL)setPlaybackRate:(float)rate;

- (PlaybackEngineState)playbackState;
- (PlaybackEngineV2Metrics)metrics;

- (void)setHealthCallback:(void (^_Nullable)(NSString* status, NSString* detail))callback;

@end

NS_ASSUME_NONNULL_END
