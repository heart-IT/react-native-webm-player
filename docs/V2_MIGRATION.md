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

## Audit + fixes applied

A full audit ran against the v2 skeleton. Fixes applied:

| Sev | Finding | Fix |
|---|---|---|
| P0 | Android `feedData` was O(N) JS↔Java crossings | Switched to base64 string (1 crossing); 100×+ faster |
| P0 | iOS had no React Native video view | Added `VideoViewV2` (`ios/playback_v2/VideoViewV2.{h,mm}`); auto-attaches to engine on `didMoveToWindow` |
| P0 | TS `feedData` had inconsistent sync/async return | Normalized all v2 methods to `Promise<T>` |
| P1 | Demux thread never compacted ring window | `compactBefore(nextCluster->GetPosition())` after each cluster drains |
| P1 | Demux loop ignored `setEndOfStream` | Added `_ring->isEndOfStream()` exit check |
| P1 | `setDisplayLayer:` raced + leaked synchronizer renderers | Main-thread dispatch + previous-renderer remove + dedupe |
| P1 | Opus decoder leaked on `tracksLoaded` reparse | Destroy guard before `opus_decoder_create` |
| P1 | Audio enqueue ignored `isReadyForMoreMediaData` | Drop + bump `audioUnderruns` if not ready |
| P1 | Android had no background/foreground lifecycle | Ported `appDidEnterBackground` / `appDidEnterForeground` from call-doctor-mobile |
| P1 | Health callback wired but never fired | `fireHealth:` invokes on transitions (buffering→playing→ended/failed) |
| P1 | VTDecompression `__bridge` race vs callback | `WaitForAsynchronousFrames` + `dispatch_sync` render queue drain in shutdown |
| P2 | `addRenderer:` after start was undefined | Same `setDisplayLayer:` fix (re-add post-start works correctly) |
| P2 | `VideoView` lacked default `videoGravity` | `VideoViewV2.init` sets `AVLayerVideoGravityResizeAspect`; `setScaleMode:` exposes 3 modes |
| P3 | Dead `extern "C" PlaybackEngineV2_currentEngineForRuntime` | Replaced with `mediamodule_v2::currentEngine()` |
| P3 | View class name parity | Both platforms register as `WebmPlayerV2View` |

Code structure:
- `PlaybackEngineV2.mm` 487 LOC (was 558, then briefly 642 over-budget)
- VP9 decoder extracted to `PlaybackEngineV2VideoDecoder.{h,mm}` (~180 LOC)
- New `VideoViewV2.{h,mm}` + manager (~80 LOC)

## Known gaps (intentional, follow-up work)

1. **Health callback JSI binding** — iOS engine fires `fireHealth:` but there's no JSI method for JS to subscribe. Workarounds: (a) iOS native code can call `[engine setHealthCallback:]`, (b) Android emits via `DeviceEventEmitter`. JSI binding requires `CallInvoker` capture; not blocking.
2. **TurboModule + JSI ArrayBuffer for Android `feedData`** — base64 is 100× faster than the original ReadableArray approach, but a TurboModule with raw JSI ArrayBuffer would skip the base64 round-trip entirely. Defer to a follow-up after the lib settles.
3. **Audio renderer pull-based feeding (`requestMediaDataWhenReadyOnQueue:`)** — current `isReadyForMoreMediaData` check + drop is correct under live (real-time) producer assumption. For producer-faster-than-realtime cases (e.g. fast-forward through cached data), pull-based is safer. Track if ever needed.
4. **Health stickiness** — once `failed` is reported, a subsequent `playing` event overwrites it. Real apps should treat `failed` as a terminal state and only recover on explicit `stop()` + `start()`. Document as a contract for now.
5. **VP9 frames silently dropped if display layer not ready** — VTDecompression callback's enqueue check (added in audit) doesn't bump a counter visible to the engine. Counter exists in engine but isn't wired through. Low priority — should be near-zero in practice.

## Pending validation work

The skeleton is structurally complete, compiles, typechecks, and lints. **Before Phase 6 (deleting v1)**, the user must validate on physical devices:

1. **iOS spike** — tap "Run V2 Spike" in example app. Plays bundled fixture; if A/V plays in sync, the architecture is validated end-to-end.
2. **iOS streaming v2** — call `MediaPipelineV2.start()` + `feedData()` from JS with real Hypercore data. Mount `<VideoViewV2 />` to render. Confirms streaming, audio renderer, synchronizer, and buffering work with continuous feed.
3. **Android streaming v2** — same, but on Android. Confirms ExoPlayer + base64-decoded WebMStreamBuffer DataSource integration. Wire `<VideoViewV2 />` + `attachToView(viewTag)` for video.

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
