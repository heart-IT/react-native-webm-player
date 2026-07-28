# Production-readiness audit

Audited at `9392713`, covering the state after P0–P3 of the Nitro rebuild.
P4 (views), P5 (health/routing) and P6 (device validation) are not yet built.

**Surface:** ~4,950 LOC — `cpp/` 2,392, `ios/` 950, `android/` 818, `tests/` 1,172,
`src/` 68. Excluded: 9,443 lines vendored libwebm, 2,080 lines nitrogen-generated.

**Verdict key:** CONFIRMED (a test fails against the unfixed code, or the defect is
unambiguous from source) · REFUTED (investigated and withdrawn) · INCONCLUSIVE
(not reachable from a host test; needs a device).

---

## Fixed during the audit

| ID   | Severity | Finding                                    | Proof                                                                                                          | Commit    |
| ---- | -------- | ------------------------------------------ | -------------------------------------------------------------------------------------------------------------- | --------- |
| P0-1 | P0       | Ring destroyed under an in-flight consumer | `tests/test_ring_lifetime.cpp` — ASan `heap-use-after-free` at `WebMStreamBuffer.cpp:335`, round 1 of 4/4 runs | `8cf54e5` |
| P0-2 | P0       | Short writes split the caller's chunk      | `tests/test_audit_proof.cpp` — 2 tests, both red pre-fix                                                       | `8cf54e5` |
| P0-3 | P0       | Raw ring handle across the JNI boundary    | `tests/test_ring_registry.cpp` — contract pinned; see scope note below                                         | `9392713` |

### P0-1 — `read()`'s fast path was invisible to the destructor

`WebMStreamBuffer::read()` has two paths. `readSlow()` took a `ConsumerActiveGuard`;
the **fast path** — taken whenever data is already available, i.e. the normal case
for a live stream — took none. The destructor waited on `consumerActiveCount_`,
which only `readSlow` incremented, so a consumer mid-`memcpy` was invisible and
`delete` freed the object under it. Both platforms reach this: the iOS demux
thread and the Android ExoPlayer loader thread both call `read()`.

**Fix:** guard moved up to cover all of `read()` and `readBatch()`. The destructor
no longer gives up after `shutdownGraceMs` — that timeout _was_ the escape hatch;
it now waits until consumers drain (bounded, because `shutdown_` is set first) and
treats `shutdownGraceMs` as the point at which it logs a contract violation.

**A second defect surfaced in the fix itself.** With the guard in place ASan was
clean but TSan still reported a race: `ConsumerActiveGuard` decremented with
`memory_order_relaxed` against the destructor's `acquire` load, so the consumer's
reads of `buffer_` were not ordered before the free. Waiting for the count was
necessary but not sufficient. The decrement is now `release`.

### P0-2 — writes were not all-or-nothing

`write()` accepted a partial chunk when space was short. These bytes feed a
container parser, so a fragment leaves a truncated element and the next write
concatenates straight onto it. The predecessor's `IngestRingBuffer` rejected whole
chunks for exactly this reason, and the project's drop-policy table says so.

`tests/test_stream_buffer.cpp` had a test asserting the truncation was **correct**
behaviour — a defect encoded as a contract. Rewritten.

### P0-3 — an integer handle has no lifetime

`nativeRead` and friends `reinterpret_cast` the handle back to a pointer. Per
media3 1.8.0 source, `ExoPlayerImplInternal.release()` blocks with a timeout and
returns `false` on expiry (`ExoPlayerImpl.release()` then only fires a `TIMEOUT`
event and returns), and `Loader.release()` shuts its executor down without
`awaitTermination`. So `stop()` cannot prove the loader stopped before it destroys
the ring.

**Fix:** `cpp/common/RingRegistry.{h,cpp}` resolves a handle to a `shared_ptr` for
the duration of a call; `release()` drops only the registry's reference. A late
call resolves to nothing and fails cleanly. The bridge now contains zero handle
casts. `nativeRead`'s `offset`/`length` are also bounded against
`GetArrayLength(dst)`, which they previously were not.

**Scope limit, stated because it is narrower than it looks.** The concurrent test
in `test_ring_registry.cpp` does **not** discriminate this design from the old one:
mutating `acquire()` to return a non-owning pointer leaves all 7 tests green,
because the P0-1 fix already covers in-flight reads. What the registry fixes is the
_after-free_ call, pinned by `acquire`-after-`release` returning null. The JNI
bridge itself needs a JVM and is **INCONCLUSIVE at host** — verified by inspection
and by the Android build linking all 9 symbols.

---

## Open findings

### Correctness · Reliability

**P1-1 · `resetStream()` corrupts the demuxer · CONFIRMED**
`ios/WebmPlaybackEngine.mm:242` clears the ring and nothing else. The demuxer is a
**local on the demux thread** (`:363`), so `WebmDemuxer::reset()`
(`cpp/demux/WebmDemuxer.h:178`) is unreachable from the JS thread and is never
called anywhere.
_Failure:_ after `resetStream()`, a new stream's EBML header is appended to a
demuxer still in `Streaming` state holding stale cluster pointers. Parse errors
follow, and the demuxer's permanent `Error` state has no recovery path.
_Fix:_ signal the demux thread with an atomic reset flag; call `demuxer.reset()`
from the loop when observed, alongside the ring clear.

**P1-2 · iOS can never report `Failed` · CONFIRMED**
`playbackState` in `ios/WebmPlaybackEngine.mm` has no branch returning
`WebmPlaybackStateFailed` — the enum case is unreachable. `result.error` from the
demuxer is logged and discarded. Android sets `failed` at four sites and maps it.
_Impact:_ both an observability hole and an iOS/Android parity break — the same
stream failure surfaces to JS on one platform and not the other.

**P1-3 · iOS decodes Opus without PLC, FEC or OSCE · CONFIRMED**
The engine calls raw libopus (`:346`, `:392`). `cpp/playback/OpusDecoderAdapter.h`
— 202 lines, ported for this purpose — is referenced **only by tests**. It sets
`OPUS_SET_COMPLEXITY(7)` (enabling OSCE Deep PLC) and provides `decodePLC()` and
`decodeFEC()`, which recovers frame N from the redundancy Opus already embedded in
packet N+1.
_Impact:_ on a lossy P2P feed, concealment data that is already present in the
stream is discarded. Android gets ExoPlayer's own concealment, so this is also a
parity gap. Also a dead-code violation.

**P2-1 · iOS video never starts if the VP9 header fails to parse · CONFIRMED**
`ios/WebmVideoDecoder.mm:92-98` creates the decode session only from
`vp9::parseHeader`. On failure there is no session, `submitFrame` returns `NO`, and
every frame is dropped — permanently, if it is the first keyframe. The container
already carries dimensions in `TrackInfo.videoWidth/videoHeight`, unused here.
_Fix:_ fall back to the container dimensions.

### iOS/Android parity

**P2-2 · Ring configuration diverges · CONFIRMED**
iOS passes no `Config` (`WebmPlaybackEngine.mm:154`) and gets every default;
Android passes v1's tuned values (`webmstream_jni.cpp:36-42`).

|                         | iOS    | Android |
| ----------------------- | ------ | ------- |
| capacity                | 32 MiB | 16 MiB  |
| min capacity            | 16 MiB | 4 MiB   |
| producer/consumer stall | 10 s   | 2 s     |
| severe backpressure     | 0.95   | 0.70    |
| batch read threshold    | 4096   | 1024    |
| shutdown grace          | 500 ms | 100 ms  |

Same stream, different buffering depth and different health thresholds. Nothing in
the design justifies it; it is an artifact of the port.

Also parity-affecting: **P1-2** (Failed unreachable on iOS) and **P1-3** (no
concealment on iOS).

### Architecture & design · React Native

**P2-3 · `src/index.ts` instantiates at module scope · CONFIRMED**
`createHybridObject` throws when the name is not registered
(`HybridObjectRegistry.cpp:71`), so `import` itself is fatal rather than catchable
— including in a Jest/Node context with no native module. It also exports a single
shared instance, which contradicts the per-instance ring the native side now has.
_Fix:_ export a factory, or a lazily-resolved getter.

### Security · Test coverage

**P2-4 · The demuxer parses untrusted input with no fuzzing · CONFIRMED**
WebM bytes arrive from remote peers, making `cpp/demux/` the primary attack
surface. The predecessor had `tests/sanitizer/fuzz_demuxer.cpp`; it was not ported.
_Fix:_ port the harness and run it in the sanitizer job.

**Engines have zero execution coverage.** The 65 host tests cover
`WebMStreamBuffer`, `WebmDemuxer` and `RingRegistry`. `WebmPlaybackEngine.mm`,
`HybridWebmPlayer.swift`, `HybridWebmPlayer.kt`, `RingDataSource.kt` and
`webmstream_jni.cpp` compile, link and resolve their symbols — but no line of any
of them has ever run. This is the single largest risk in the project and only P6
closes it.

### Dependency health

**Low exposure, one real risk.** 32 advisories (15 high, 17 moderate) across 1,273
packages — **all in devDependencies**. `dependencies` is empty, so nothing reaches
consumers. The genuine exposure is the publish pipeline (`semantic-release` and its
tree run with an npm token). _Fix:_ pin/refresh the release toolchain; keep runtime
deps at zero.

### Documentation

**P2-5 · `README.md` is scaffold boilerplate · CONFIRMED**
Credits the wrong author and repository, and describes "a react native package
built with Nitro" rather than this player.

### CLAUDE.md compliance

Audited against the v2 `CLAUDE.md` authored at the start of this audit (the
predecessor's document describes v1 subsystems this project deleted).

| Rule                           | Status                                                                                                               |
| ------------------------------ | -------------------------------------------------------------------------------------------------------------------- |
| 600-line hard ceiling          | ✅ no file over 600                                                                                                  |
| 400-line warning               | ⚠️ 4 files (`test_stream_buffer` 501, `test_e2e_demux` 485, `WebmPlaybackEngine.mm` 445, `WebmStreamBuffer.cpp` 505) |
| No `TODO`/`FIXME`              | ✅ (only vendored libwebm)                                                                                           |
| No dead code                   | ❌ `OpusDecoderAdapter` — see P1-3                                                                                   |
| Spec as parity contract        | ✅ both platforms declared; nitrogen enforces                                                                        |
| Zero runtime deps              | ✅                                                                                                                   |
| CI on all branches, unfiltered | ✅                                                                                                                   |

---

## Coverage of this audit

Stated plainly so the gaps are visible rather than implied.

| Dimension                      | Coverage                                                                                                                                                           |
| ------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Correctness                    | Partial — shared C++ and both engines read; no systematic sweep of `WebmBlockParser` internals                                                                     |
| Bugs & edge cases              | Partial                                                                                                                                                            |
| Reliability & resilience       | Partial — recovery paths reviewed; failure injection not performed                                                                                                 |
| Architecture & design          | Covered                                                                                                                                                            |
| iOS/Android parity             | Covered                                                                                                                                                            |
| Security                       | Partial — attack surface identified, no fuzzing run, no threat model                                                                                               |
| Test coverage                  | Covered (as an assessment; gaps not closed)                                                                                                                        |
| Code quality & maintainability | Partial                                                                                                                                                            |
| React Native best practices    | Partial — JS surface only; no render/bridge profiling                                                                                                              |
| CLAUDE.md compliance           | Covered                                                                                                                                                            |
| Dependency health              | Covered                                                                                                                                                            |
| Observability                  | Partial — P1-2 found; no systematic review of what JS can see                                                                                                      |
| **Performance**                | **Not audited** — needs a device; no measurements taken                                                                                                            |
| **P2P compliance**             | **Not audited** — no P2P code in this repo; the contract with the Bare feeding layer (backpressure, EOS, reconnect) is undefined and should be specified before P6 |

**Not auditable here:** anything requiring real playback — renderer readiness,
audio focus, route changes, decoder behaviour, A/V sync, memory under load. All
deferred to P6.

---

## Recommended order

1. **P1-1** `resetStream` — a documented API that corrupts state.
2. **P1-2** iOS `Failed` path — failures are currently invisible to JS.
3. **P1-3** wire `OpusDecoderAdapter` in — removes dead code and restores concealment.
4. **P2-1** VP9 dimension fallback — a parse failure currently means no video at all.
5. **P2-3** `src/index.ts` factory — import should not be fatal.
6. **P2-2** ring config parity.
7. **P2-4** port the fuzz harness.
8. **P2-5** README.
