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
| 0 | Snapshot legacy + branch v2 | ✅ done (`legacy-v1-final` tag, `refactor/v2-architecture` branch pushed) |
| 1 | iOS spike (validate VP9 path) | ✅ code committed (`ios/spike/`); awaiting physical-device validation |
| 2 | Shared C++ slim | ✅ done — WebMStreamBuffer ported to `cpp/common/`. Deletions deferred to Phase 6 |
| 3 | iOS rebuild | ✅ skeleton done — engine + JSI committed to `ios/playback_v2/`. VideoView wiring pending (Phase 3.5) |
| 4 | Android rebuild | ✅ skeleton done — `android/.../v2/StreamPlayerModule.kt` + view + JNI bridge committed |
| 5 | TypeScript API cleanup | ✅ partial — `src/v2.ts` added with v2 surface. v1 `src/index.tsx` stays until Phase 6 |
| 6 | Delete legacy + docs | pending — final cleanup PR after device validation |

## Pending validation work

The skeleton is structurally complete and compiles. **Before Phase 6 (deleting v1)**, the user must validate on physical devices:

1. **iOS spike** — tap "Run V2 Spike" in example app. Plays bundled fixture; if A/V plays in sync, the architecture is validated end-to-end.
2. **iOS streaming v2** — call `MediaPipelineV2.start()` + `feedData()` from JS with real Hypercore data. Confirms streaming, audio renderer, synchronizer, and buffering work with continuous feed.
3. **Android streaming v2** — same, but on Android. Confirms ExoPlayer + WebMStreamBuffer DataSource integration.

## Phase 3.5 — iOS VideoView (TBD)

`PlaybackEngineV2.setDisplayLayer:` accepts an `AVSampleBufferDisplayLayer`, but the lib doesn't yet expose a React Native view component that creates one and registers with the engine. Options:

1. Add `ios/playback_v2/VideoViewV2.{h,mm}` + `VideoViewV2Manager.mm` (parallel to v1 `ios/video/VideoView.mm`); ~150 LOC.
2. Reuse v1 `VideoView.mm` and have it install its layer into the engine's `setDisplayLayer:`.

Pick after device-testing audio-only playback works, since audio validates the synchronizer architecture without needing the view layer.

## Phase 6 deletion list

Once v2 is verified and the app is migrated to import from `@heartit/webm-player/src/v2`:

```
cpp/pipeline/                    # entire dir
cpp/playback/                    # entire dir except OpusDecoderAdapter.h
cpp/video/                       # entire dir except VP9HeaderParser.h, VideoConfig.h
cpp/transcript/                  # entire dir (transcription deferred)
cpp/common/{AVSyncCoordinator,JitterEstimatorBase,ArrivalConfidence,HealthWatchdog,
            StallRecoveryController,ClipIndex,IngestRingBuffer,IngestThread,
            RouteHandler,AudioRouteTypes,MediaSessionBase,AudioOutputBridgeBase,
            OrchestratorLifecycle,PipelineOrchestrator,ModuleRegistry,JsiDispatchTable,
            SessionMetrics,ThreadAffinity,TimedJoin,WorkerThread,AudioResampler,
            MediaSessionLifecycle.inl,MediaSessionMetricsImpl.inl,
            AudioOutputBridgeRender.inl,AudioOutputBridgeRestart.inl,
            CompilerHints.h,bindings/}.h
ios/playback/                    # entire dir
ios/transcript/                  # entire dir
android/src/main/jni/playback/   # entire dir
android/src/main/jni/video/MediaCodecDecoder.cpp
android/src/main/jni/transcript/ # entire dir
android/src/main/jni/audiosession_jni.cpp  # if v2 doesn't need it
src/index.tsx                    # rewrite to re-export from src/v2
```

After deletion, rename `__MediaPipelineV2` → `__MediaPipeline`, `WebmPlayerV2Module` → `WebmPlayerModule`, `WebmPlayerV2View` → `WebmPlayerVideoView`.

## File budget after v2 (estimate)

| Surface | LOC | Notes |
|---|---|---|
| Shared C++ | ~2,000 | demux + WebMStreamBuffer + utilities |
| iOS | ~1,500 | engine + JSI + view |
| Android | ~700 | StreamPlayerModule + view + JNI |
| TypeScript | ~150 | v2-only surface |
| **Total** | **~4,400** | vs v1's ~20,200 (4.6× smaller) |

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
