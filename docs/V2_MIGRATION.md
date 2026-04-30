# V2 architecture migration

Tracking doc for the v1 → v2 rewrite. v1 is preserved at tag `legacy-v1-final`. v2 work happens on branch `refactor/v2-architecture`.

## Why

v1 reimplements ExoPlayer in C++ with ~20K LOC of custom orchestration (sync coordinator, drift comp, jitter buffer, health watchdog, recovery state machine, custom mixer, RT-safe queues, etc.). On Android this is unnecessary — ExoPlayer does it natively. On iOS, modern AVFoundation provides `AVSampleBufferRenderSynchronizer` + `AVSampleBufferAudioRenderer` + `AVSampleBufferDisplayLayer`, which together replace the entire custom A/V sync and audio output stack with platform-managed alternatives.

v2 is asymmetric by design:
- **Android**: ExoPlayer + thin `WebMStreamBuffer` shim (port of `call-doctor-mobile` architecture).
- **iOS**: libwebm + libopus + VTDecompression feeding `AVSampleBufferRenderSynchronizer`. Platform owns A/V sync, audio routing, route changes.
- **Shared**: `WebmDemuxer` (post wedge fix), `WebMStreamBuffer` (lock-free SPSC byte ring), JSI surface.

Target: ~6K LOC (3× smaller than v1), fewer bug classes, leans on platforms.

## Phase status

| # | Phase | Status |
|---|---|---|
| 0 | Snapshot legacy + branch v2 | **in progress** (local done; push pending user approval) |
| 1 | iOS spike (validate VP9 path) | pending |
| 2 | Shared C++ slim | pending |
| 3 | iOS rebuild | pending |
| 4 | Android rebuild | pending |
| 5 | TypeScript API cleanup | pending |
| 6 | Delete legacy + docs | pending |

## VP9 path question (open until Phase 1)

Two candidate paths:
1. **Direct**: build `CMSampleBuffer` from compressed VP9 + vpcC, enqueue into `AVSampleBufferDisplayLayer` post `VTRegisterSupplementalVideoDecoderIfAvailable`. Confidence ~60%.
2. **Fallback**: VTDecompression → `CVPixelBuffer` → `CMSampleBuffer` → enqueue. Confidence ~95%; +200 LOC.

Spike picks one. Either way the synchronizer architecture stands.

## What v2 drops vs v1

- DVR / clip capture / seek (`captureClip`, `seekTo`, `setClipBufferDuration`, `getBufferRangeSeconds`)
- On-device transcription (`setTranscriptionEnabled`, `setTranscriptCallback`, etc.) — can return as separate optional module post-v2
- `setBufferTarget`, `setCatchupPolicy` (platform defaults instead)
- Most `HealthEvent.metrics` fields (keep ~6 essential ones)

## What v2 keeps stable

JSI/TypeScript core: `start/stop/pause/resume/feedData/setMuted/setGain/setPlaybackRate/getMetrics/getCurrentTimeUs/setStreamStatus/setHealthCallback/setAudioRoute/getCurrentAudioRoute/getAvailableAudioRoutes`.

App should require zero changes for the core playback path.

## Files preserved from v1

- `cpp/demux/Webm*` (post wedge fix)
- `cpp/playback/OpusDecoderAdapter.h` (decoupled from pipeline types)
- `cpp/common/{MediaConfig,JsiUtils,JsiDispatchTable,Platform,MediaTime,MediaLog}.h` (slimmed)
- `cpp/video/VP9HeaderParser.h` (vpcC builder)
- `ios/video/VP9Decoder.{h,mm}` (post vpcC + register fix; needed for fallback path)
- `ios/opus/`, `ios/whisper/` vendored
- `src/index.tsx` (slimmed surface)

## Files deleted in v2

`cpp/pipeline/` entirely. `cpp/common/` orchestration (`PipelineOrchestrator`, `MediaSessionBase`, `AudioOutputBridgeBase`, `AVSyncCoordinator`, `JitterEstimatorBase`, `ArrivalConfidence`, `HealthWatchdog`, `StallRecoveryController`, `ClipIndex.{h,cpp}`, `IngestRingBuffer`, `IngestThread`, `RouteHandler`, `AudioRouteTypes` partial). `cpp/playback/` except `OpusDecoderAdapter`. `cpp/video/` except `VP9HeaderParser`, `VideoConfig` (slim). `cpp/transcript/` entirely. `ios/playback/AVAudioOutputBridge.mm`, `ios/playback/MediaSession.h`, focus state machine. `android/src/main/jni/playback/`, `jni/video/MediaCodecDecoder.cpp`, native demux duplication.
