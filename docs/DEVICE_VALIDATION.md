# Device validation (P6)

The example app (`example/App.tsx`) auto-plays a bundled 1-second 480p VP9 +
5.1 Opus fixture on launch and shows live metrics, so a run needs no interaction.
Buttons are there for manual checks.

```bash
npm install
cd example && npm run ios      # or: npm run android
```

## Physical device: validated

**iPhone 11 (A13), iOS 26.5.2, Release build.** After the fixes below, video
renders, audio is audible, and the packet counts and media position all read
correctly — matching Android on the same fixture.

Two assumptions from the simulator round turned out to be **wrong**, and only the
device disproved them:

- _"The A13 has no VP9 hardware decoder."_ It does. VideoToolbox decoded all 24
  frames. The black screen was ours: decoded frames were being discarded after
  decode, at the display layer.
- _"Audio is barely decoding."_ Audio was playing correctly the whole time; the
  on-screen count was a stale mirror that stopped updating once feeding ended.

## Sustained playback (60s)

A second fixture — 60s, 512x288 VP9 + stereo Opus, with a burnt-in timecode and
a beep on every second boundary — covers sustained arrival rather than a
one-shot decode. Confirmed on the iPhone 11: video plays, beeps are audible.

**Result: sync holds for the full 60s of content.** Drift stays near zero for the
whole clip on the device.

Getting a trustworthy number out of this took two harness fixes, and neither bug
was in the player. Both are worth knowing about, because both produce a large
negative drift that looks exactly like a failing player:

1. **Starved input.** The feeder computed a byte budget per timer tick, but
   `setInterval` fires late under JS load and a per-tick budget never makes that
   up. It delivered ~74% of real-time rate, so the player ran out of data and
   drift grew to -8.4s over a minute. The feeder now derives its target from
   elapsed time and self-corrects, which tracks 99.9% of real-time rate.
2. **Measuring past the end of the clip.** Media time stops when the clip ends;
   wall time does not. On a 60s clip left running to wall=100s, the reported
   drift was -40s — which is not drift at all, just the 40s that elapsed after
   playback finished. The measurement now clamps its reference to the clip
   duration and marks the run `ended`.

The lesson for reading this metric: `drift` is only meaningful while content is
still arriving _and_ still playing. Outside that window it measures the harness.
Check `fed` against elapsed time, and check whether the clip has ended, before
concluding anything about playback.

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

### iOS — simulator run (superseded by the device run above)

```
fed 45863B · audio 1 (recovered 0, underruns 0) · video 0 (dropped 24)
854x480 · t=10.14s
health: 0 buffering → 1 first audio frame → 2 end of stream
```

Confirmed working: HybridObject + view registration, `createWebmPlayer`,
`feedData` (all 45863 bytes accepted), the ring, the demuxer (854x480 is read
from the container), track parsing, health transitions, `setEndOfStream`, route
reporting, and multichannel Opus decode.

Three things looked unresolved here; the device settled all three, and each
turned out to be a real defect rather than a simulator artifact:

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

## Background and foreground

Policy: **playback pauses on background and resumes on return.** No configuration
is required from a consuming app — no `UIBackgroundModes`, no Android foreground
service. The library observes the transition itself (`UIApplication` notifications
on iOS, React Native's host lifecycle on Android).

What the transition has to survive, and why each piece is there:

- **iOS revokes video decoder resources when the app leaves the foreground**, and
  the `VTDecompressionSession` stays invalid afterwards. Nothing reported this, so
  a session that outlived a background trip was reused forever and never decoded
  another frame. The session is now released on suspend and rebuilt at the first
  keyframe after resume, and `kVTInvalidSessionErr` is treated the same way if it
  turns up by another route.
- **The synchronizer's timebase is host-clock-based** and keeps advancing while
  the process is suspended. Left running, a 30s background trip would return 30s
  ahead of the audio that actually played, so the rate goes to 0 on suspend.
- **Stopping the renderer is not enough.** The demux thread would keep draining
  the ring and decoding into a queue nobody consumes, and the audio decoder drops
  what does not fit. Measured on the simulator: a 12s background trip discarded
  **401 audio packets** — a silent hole on resume. The demux thread is now parked,
  so the bytes stay in the ring. The same fix applies to an explicit `pause()`,
  which had the same hole.
- **A `resume()` from JS while backgrounded does not start playback** on either
  platform. The decoder has no session and the audio session may be inactive.

Verified on the iOS simulator across a 12s background trip: audio packets
discarded went from 401 to **0**, drift +0.07s, playback continued normally.

The simulator does not suspend the process, so it cannot settle whether the media
clock survives a real suspension — that needs the device, and is listed below.

### OPEN: video does not return after a background trip (iOS device)

**Status: unresolved.** Confirmed twice on the iPhone 11: after backgrounding for
~30s, audio resumes but the picture stays frozen. Everything below is what has
been ruled in or out, so the next session does not repeat it.

Ruled **out**:

- _Not a keyframe famine._ The 60s fixture has **30 keyframes, one every 2s**
  (`ffprobe` over the decoded fixture, counted via named JSON fields — a CSV
  query gave a wrong answer first because ffprobe emits fields in its own order,
  not the requested one). The decoder had a recovery point every 2 seconds.
- _Not the `ensureSession` early-return._ `shutdownSession` zeroes
  `_width/_height/_profile`, so the guard cannot return a stale session.

Ruled **in** (partially): the display layer is left in
`AVQueuedSampleBufferRenderingStatusFailed` after the app loses the foreground,
and it only leaves that state on `-flush`. The flush inside `feedLayer` cannot
do it, because a failed layer never becomes ready for more media data and so the
`requestMediaDataWhenReadyOnQueue:` block it runs from is never invoked again.

An explicit flush + re-arm on resume was written (uncommitted, in
`WebmVideoDecoder.resume`) and **did not fix it**, so at least one more cause is
in play. Candidates not yet tested:

1. The `AVSampleBufferRenderSynchronizer` may need the layer re-added as a
   renderer after the failure, not just flushed.
2. `_pending` frames enqueued before the trip carry timestamps behind the
   resumed timebase and may all be discarded as late, with newly decoded frames
   never arriving because the VP9 session did not actually rebuild.
3. The VP9 session rebuild itself is unverified — no evidence either way yet.

Blocking the diagnosis: **no working device log capture.** `MEDIA_LOG` goes to
`os_log` (subsystem `com.heartit.webmplayer`), and neither route worked:
`idevicesyslog` reads the legacy syslog and captured 339 lines with none of ours;
`log stream --device-name` does not exist on this macOS; `devicectl device
console` is not a subcommand, and `process launch --console` forwards stdio,
which `os_log` does not use. Next attempt: Console.app with the device selected,
or `sudo log collect --device-udid <udid>` and query the archive. Getting logs
working is the first step — the fix attempts so far have been blind.

Note for whoever picks this up: `log` is aliased to `git log` in this shell, so
scripted captures must use `/usr/bin/log`.

### Known limitation: what `currentTimeSeconds` measures

It is the presentation timestamp of the last buffer _handed to the renderer_
(`WebmAudioDecoder.mm`, stored at `enqueueSampleBuffer:`), not what has been
heard. It therefore runs ahead of real playback by the renderer's buffer depth,
and after a pause it catches up to wall time as the backlog is enqueued, faster
than the audio can actually play. Good enough to spot a stall; not a playback
position.

## Checklist for a physical device

Run the example and work through these. Anything that fails is a real defect —
the simulator caveats above do not apply.

**iOS**

- [x] Video renders (this settles finding 2 above)
- [x] Audio is audible, and `audio N` climbs past 1 (settles finding 1)
- [x] `t=` tracks media position, not wall-clock (settles finding 3)
- [x] A/V stay in sync across a 60s continuous feed
- [ ] Backgrounding pauses, foregrounding resumes without artefacts
      (simulator-verified; the device must confirm the media clock does not
      jump across a real process suspension)
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
