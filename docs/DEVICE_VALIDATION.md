# Device validation

The example app joins a broadcast by key. Broadcast the 60 s VP9 + stereo Opus
fixture from the Mac with `sh scripts/dev-broadcast.sh`, paste the printed key
into the app, tap Play. For unattended runs put the key in
`example/src/devKey.ts` and the app plays it on launch. See `example/README.md`.

## Reading logs

- **iPhone, no root needed.** Launch from the Mac with the stderr mirror on:

  ```bash
  xcrun devicectl device process launch --console \
    --environment-variables '{"WEBM_LOG_STDERR":"1"}' --device <udid> com.webmplayerexample
  ```

  Every `MEDIA_LOG` line reaches that console with a millisecond timestamp:
  one line per health transition, the VP9 session, the first presented frame
  and every 300th after it, and the display layer's own failure reason.

- **Simulator.** `/usr/bin/log stream --style compact --predicate 'process == "WebmPlayerExample"'`
  on the Mac. The worklet's console (vcr-core's "seeking to N") appears under
  the `(BareKit)` prefix. `log` is aliased to `git log` in this shell.
- **Android.** `adb logcat -s WebmPlayer ReactNativeJS`.

## Verified 2026-10-09, key-in flow, React Native 0.87.1 / Nitro 0.37.1

**iPhone 11 (A13), iOS 26.6.1, Release, broadcast from the Mac over
Hyperswarm, run unattended from the Mac.** Timeline from the console, in
seconds after the first log line:

| t    | event                                                                                                                         |
| ---- | ----------------------------------------------------------------------------------------------------------------------------- |
| 0.4  | worklet started, `joining`                                                                                                    |
| 13.7 | swarm connection                                                                                                              |
| 21.7 | first audio frame, VP9 session 512x288, first video frame presented, within 20 ms of each other                               |
| 25.1 | 82 frames presented, 0 dropped                                                                                                |
| ~24  | Settings brought to the front: app in background                                                                              |
| 26.4 | VT session invalidated by iOS; rebuilt at the next keyframe at 28.2 (48 frames dropped in between, the only drops of the run) |
| 35.1 | display layer failed in the background; `requiresFlushToResumeDecoding` fired, layer flushed                                  |
| ~45  | app back in the foreground                                                                                                    |
| 50.2 | 600 frames presented                                                                                                          |
| 55.3 | 714 presented, 48 dropped, 1,725 audio packets                                                                                |
| 57.6 | broadcast ended, player stopped                                                                                               |

This is the scenario of the old frozen-picture bug, observed with logs: the
layer fails while the app is in the background, the documented notification
arrives, and after the flush frames keep reaching the layer through the
return. Not verified: pixels on the glass after the return, which needs eyes
on the phone. An earlier run of the same session, before the worklet started
the mux at a keyframe, showed 38 frames dropped at the join: those were the
P-frames between the join and the first keyframe, which no decoder can use.
Drops reported by `videoFramesDropped` on iOS are frames submitted without a
usable decode session, so after iOS revokes the session in the background
they count up until the next keyframe.

**Android, OnePlus 8 (API 33, arm64), debug.** Joined in 4 s, data 1 s later,
played the full clip at 512x288 with audio and video counters climbing
together and 0 frames dropped, then `ended` took the player to Idle. Run six
times across the session; the two failures seen along the way were real
defects, both fixed: a join that seeked to the start of the log when the
length was not yet known, and an ExoPlayer container error latching `Failed`
on a session that then played.

**iOS simulator, iPhone 17 (iOS 26).** Same flow; audio plays, video is
dropped for lack of a VP9 decoder, `ended` stops the engine. The simulator
settled the bare-kit assumptions: sync `read` returns nil when nothing is
ready and empty at end of stream, `readable` keeps firing after a nil read,
and our own reader thread may call `read`.

### What the runs taught about joining

- Swarm discovery through the DHT took 4 to 12 s on fresh keys and up to 45 s
  on a key whose topic carried stale announcements from killed broadcasters.
  A restarted broadcaster is found on the DHT's schedule, not ours.
- vcr-core's `seek(-1)` lands on the length known at that instant. The worklet
  now waits for a connection and its length proof, then re-seeks until it is
  within 50 fragments of the tail, before muxing a byte.
- Lag is measured against the log's append rate over 5 s, not fragment
  durations: a video-only fragment has none.

### What the iPhone run of 2026-10-09 said about the margin

The foreground run before pacing (2 s margin, queues unpaced) connected
through the router's public address and presented 300 video frames in the
first 30 s of a 24 fps clip, with 513 frames and 513 audio packets dropped at
the decoders' pending caps and two "stream stalled" reports, while bytes
arrived at the clip's bitrate over the whole window. That is a bursty path:
the data comes in bursts, each one overflows the decoded queues, and the gap
after it drains them. The margin has to sit in the ring, where nothing is
dropped, and has to be longer than the gaps. Pacing (`paceOn:` in
WebmDemuxPump.mm) does the first; the live delay in WebmPlaybackPolicy.h is the
second. The simulator reproduces the pattern with the recorder frozen for 3 s
in every 8 (`kill -STOP`): at 1.5 s the pump counted 4 underruns and the
engine 4 stalls in 50 s; at 4 s, none, with the extra 2.5 s visible as ring
occupancy. 4 s is the working value; two Release builds (1.5 s and 4 s) are
queued for the next unlock of the phone to confirm it on the real path. The
worklet's lag threshold (10 s) stays above either.

### What the iPhone run of 2026-10-12 said about the hangs

With the 4 s margin the phone played the full 24 fps with no underrun and no
drop before the display layer, and the picture still hung for about half a
second every few seconds. The layer's own counters (`AVVideoPerformanceMetrics`
through `sampleBufferRenderer`, logged beside the pump summary) showed 302 of
3865 frames dropped as late, and the lead times added to the decoder showed
frames leaving VideoToolbox 0.5 s behind the clock while the pull path added
nothing. The cause was the pump: it submitted every audio packet of a 64 KB
chunk (about 2.5 s of media, paced on the renderer) before any video frame of
that chunk, so each chunk's first frames reached the decoder after their time.
Submission is now one pass in presentation order across both tracks. Same
path, same clip, 2 minutes: 2331 frames displayed, 0 dropped, 0 late, decode
lead steady at 1.0 s.

A finished broadcast joined after the fact used to leave the player buffering
for good (simulator, 2026-10-10: connected, 0 bytes, no `ended`). The worklet
now checks vcr-core's session liveness once the length is known and reports
`ended` straight away.

### What the iPhone run of 2026-10-12 said about the parse window

The final-tree run of the same day stopped at 100 s: the pump kept reading
bytes, the demuxer dropped every feed as "demuxer buffer overflow", the audio
and video counts froze, and nothing rejoined. The demuxer's 2 MiB parse window
had filled. The block parser's two "need more data" exits returned before the
compaction schedule at the end of `parseBlocks`, and on a live feed every chunk
ends mid-cluster, so the window was never trimmed and filled after about 2.1 MB
of stream, 80 s at this bitrate. Reproduced offline by feeding the worklet's
own remux of the 5-minute fixture in 64 KB chunks: overflow at feed 32 with
2.16 MB in. With both exits routed through the compaction schedule the full
7.9 MB demuxes with the window peaking at 94 KB. The regression test is
`mid_cluster_feeds_keep_the_window_trimmed` in `tests/test_demux_hostile.cpp`.
Two things the run also showed stay open: the overflow error is not terminal
for the pump (no health change, no rejoin), and compaction trims to libwebm's
Segment-relative cluster position, the EBML and Segment headers short of the
cluster. The fixed tree ran on the phone the same afternoon: the swarm join
took 84 s this time, so the 3-minute window held 89 s of playback, 2.30 MB in,
past the point where the previous run filled the window, with 2042 frames
displayed, 0 dropped, 0 late, 0 underruns and no window-full line.

### A startup crash at the 16.6 deployment target (2026-10-12)

With the podspec and example at iOS 16.6, the Release build crashed at
`play()` on the iPhone 11 in 2 of 6 launches: SIGTRAP, and both crash reports
show the engine's `play` returning 0x18 bytes past its true return address
into the Swift witness, so its saved return address had been corrupted. The
same tree at 15.1 never crashed, the 16.6 build under Address and Undefined
Behavior Sanitizers played cleanly, 13 Release launches on the simulator were
clean, and three debugger-attached launches with a hardware watchpoint on the
slot saw no stray write and no crash (the debugger changes the timing). The
only compile difference is the target triple. What the crash window did hold
was a race: the view's `setDisplayLayer` runs on main and writes
`_displayLayer`, `_synchronizer` and `_videoDecoder` while `play()` creates
and adds the same objects on the JS thread under `_control`, which
`setDisplayLayer` did not take. It now does. After that fix 6 of 6 launches at
16.6 started cleanly. That is consistent with the race being the cause but not
proof of it: a 1-in-3 crash clears six launches about 9 % of the time, so the
item stays on the hardware list until more launches accumulate.

## Still to run on hardware

- [ ] The 16.6 startup crash: keep counting launches of the 16.6 Release
      build. A recurrence means the display-layer race was not the cause; the
      next step is then a Debug device build or bisecting the target.

- [ ] Pixels after a background trip, with eyes on the phone.
- [ ] The 2 MiB ring (round 5, row 4) and the "window N KB" figure in the
      pump's 5 s summary (row 5): ring occupancy stays well under 100% and the
      window stays flat across a 3-minute run.
- [ ] `stop()` then `play()` again; memory flat across three cycles.
- [ ] Phone call during playback: Buffering "audio interrupted", then rejoin.
- [ ] Player stalled for 10 s (debugger pause): worklet reports `lag ...`,
      native rejoins at the live edge.
- [ ] Broadcaster restarts into the same key: worklet reports `reconfigured`,
      native rejoins (the 60 s fixture is too short for the unattended script
      to reach this step).
- [ ] Network off for 20 s: "stream stalled" once the renderer runs dry (round
      5, row 10: it now fires on the pump's dry transition, not after 2 s of
      empty reads), "stream resumed" and playback on reconnect.
- [ ] Video sample duration from the container's DefaultDuration (row 11,
      24 fps on the remuxed stream): frames presented, none dropped as late.
- [ ] Android: audio focus loss and regain.
- [ ] The 7 s between the swarm connection and the first audio frame on the
      iPhone, against 1 s on the simulator and 3 s on Android: measure where it
      goes.
