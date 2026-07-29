# Device validation (P6)

The example app (`example/App.tsx`) auto-plays a bundled 1-second 480p VP9 +
5.1 Opus fixture on launch and shows live metrics, so a run needs no interaction.
Buttons are there for manual checks.

```bash
npm install
cd example && npm run ios      # or: npm run android
```

## What has been executed, and where

Runs were done on the **iOS Simulator (iPhone 17, iOS 26.4)** and the
**Android emulator (API 36)**. Neither substitutes for a physical device, but
both executed real code — before this, nothing in the project had ever run.

### Android — fully working

```
fed 45863B · audio 51 (recovered 0, underruns 0) · video 24 (dropped 0)
854x480 · t=1.01s
health: 0 buffering → 1 ready → 2 end of stream
```

Every audio packet and every video frame decoded, nothing dropped, correct frame
size, correct media position, and a decoded frame visibly on screen. The ring,
demuxer, JNI bridge, ExoPlayer DataSource, view and health path are all confirmed
end to end.

### iOS — partially working

```
fed 45863B · audio 1 (recovered 0, underruns 0) · video 0 (dropped 24)
854x480 · t=10.14s
health: 0 buffering → 1 first audio frame → 2 end of stream
```

Confirmed working: HybridObject + view registration, `createWebmPlayer`,
`feedData` (all 45863 bytes accepted), the ring, the demuxer (854x480 is read
from the container), track parsing, health transitions, `setEndOfStream`, route
reporting, and multichannel Opus decode.

Three things remain unresolved and **need a physical device**:

1. **Audio reaches the renderer but only one buffer is pulled.** Underruns are 0
   and the queue never saturates, so buffers are being produced and queued; the
   renderer simply stops asking after the first. This may be the simulator not
   running a real audio device, or a genuine bug in the pull loop. Android
   decodes all 51 packets from the same fixture, so the fixture and demuxer are
   not the cause.
2. **All 24 video frames dropped.** Expected on the simulator, which has no VP9
   hardware decoder — `VTIsHardwareDecodeSupported('vp09')` should be false, so
   no decode session is created and `submitFrame` returns NO. Unverified until a
   device runs it.
3. **`currentTimeSeconds` reports wall-clock, not media position** (10.14s for a
   1-second fixture). Android reports 1.01s correctly, so this is iOS-specific:
   `AVSampleBufferRenderSynchronizer.currentTime` advances from zero at the set
   rate regardless of what has been enqueued. Needs a device to confirm the fix
   shape before changing it.

## Checklist for a physical device

Run the example and work through these. Anything that fails is a real defect —
the simulator caveats above do not apply.

**iOS**

- [ ] Video renders (this settles finding 2 above)
- [ ] Audio is audible, and `audio N` climbs past 1 (settles finding 1)
- [ ] `t=` tracks media position, not wall-clock (settles finding 3)
- [ ] A/V stay in sync across a 60s continuous feed
- [ ] Backgrounding pauses, foregrounding resumes without artefacts
- [ ] Route change (speaker ↔ wired ↔ Bluetooth) keeps playback going and fires
      `setRouteChangeCallback`
- [ ] `stop()` then `start()` cycles cleanly, with no leak across cycles
- [ ] `resetStream()` mid-feed recovers rather than wedging the demuxer

**Android**

- [ ] Same list. The emulator already passes the first three.
- [ ] Audio focus: a phone call or another media app pauses/ducks correctly

**Both**

- [ ] Sustained feed from real Hypercore data, not the fixture
- [ ] Memory flat over several minutes (no growth per stream cycle)
- [ ] `audioFramesRecovered` climbs on a lossy link — exercises the FEC/PLC path,
      which no test covers today

## Known limitation

`audioFramesRecovered` is always 0 on Android: ExoPlayer conceals internally and
does not report recovered frames. The counter is meaningful on iOS only.
