# Sanitizer Tests (C++)

Standalone C++ tests that verify the shared `cpp/` code for memory safety, threading correctness, undefined behavior, and pipeline logic. These run on macOS using Clang sanitizers -- no React Native or device required.

## Prerequisites

- macOS with Apple Clang (Xcode Command Line Tools)
- CMake 3.20+
- Opus library: `brew install opus`
- SpeexDSP library: `brew install speexdsp`

## Running

```bash
cd tests/sanitizer

# AddressSanitizer + LeakSanitizer
cmake -B build -DSANITIZER=address -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
./build/test_lsan
./build/test_ubsan
./build/test_route_handler
./build/test_e2e_demux
./build/test_temporal_stress
./build/test_health_watchdog

# ThreadSanitizer
cmake -B build_tsan -DSANITIZER=thread -DCMAKE_BUILD_TYPE=Debug
cmake --build build_tsan -j --target test_tsan
./build_tsan/test_tsan
```

Or use the yarn shortcuts from the project root:

```bash
yarn test:asan    # ASan build + run
yarn test:tsan    # TSan build + run
yarn test:ubsan   # UBSan build + run
```

## Test Binaries

| Binary                 | Sanitizer   | Tests | What it covers                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           |
| ---------------------- | ----------- | ----- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `test_lsan`            | ASan + LSan | 30    | FramePool acquire/release/RAII, FrameQueue SPSC push/pop/clear, JitterEstimator lifecycle, DriftCompensator/Speex resampler, CatchupController, AudioMixer, DecodeThread start/stop, OpusDecoderAdapter init/decode/PLC/move, pool stress cycles, ClipBuffer enable/disable/extract lifecycle, StallRecoveryController reset cycles                                                                                                                                                                                                      |
| `test_tsan`            | TSan        | 36    | Concurrent FramePool (4 threads), SPSC producer/consumer, JitterEstimator concurrent update/read, DriftCompensator concurrent, AudioDecodeChannel concurrent push+read+decode, AudioMixer concurrent access, DecodeThread concurrent push, StreamMetrics concurrent counters, VideoFrameQueue concurrent push/pop and reset-during-push, VideoJitterEstimator/VideoSurfaceRegistry concurrent access, ClipBuffer concurrent append+extract, StallRecoveryController concurrent stall-and-recover, AudioLevelMeter concurrent update+read |
| `test_ubsan`           | UBSan       | 86    | Pool boundary conditions, JitterEstimator arithmetic edge cases, DriftCompensator division-by-zero/extreme-drift, Speex resampler extreme ratios, CatchupController bounds, AudioMixer null/zero/gain-ramp, AudioDecodeChannel inactive-read/push/zero-size, OpusDecoderAdapter null/DTX/garbage, VideoFrameQueue keyframe/oversize/overflow, AudioLevelMeter silence/sine/clipping/dBFS, ClipBuffer lifecycle/wrap-around/extractFromPts, StallRecoveryController state transitions, WebmDemuxer parse edge cases                       |
| `test_route_handler`   | ASan        | 18    | RouteHandler: initial detection, dedup, restart matrix, builtin/A2DP/SCO/Wired/USB transitions, drift reset, callback firing                                                                                                                                                                                                                                                                                                                                                                                                             |
| `test_e2e_demux`       | ASan        | 27    | End-to-end WebM demuxer: real stream parsing, track extraction, incremental feeding, reset/re-feed, corrupt data handling                                                                                                                                                                                                                                                                                                                                                                                                                |
| `test_temporal_stress` | UBSan       | 29    | Behavioral pipeline verification under temporal stress: burst arrivals, starvation, PTS jumps, jitter spikes, speculative-mode handling, mutation regression                                                                                                                                                                                                                                                                                                                                                                             |
| `test_health_watchdog` | None        | 55    | HealthWatchdog state machine: all transitions, trigger conditions, dedup, rate limiting, evaluation window, demuxer-wedge stalled path                                                                                                                                                                                                                                                                                                                                                                                                   |
| `test_ingest_thread`   | ASan        | 15    | IngestRingBuffer + IngestThread: SPSC streaming, IMkvReader integration, wake/drain semantics                                                                                                                                                                                                                                                                                                                                                                                                                                            |
| `test_audit_proof`     | ASan/TSan   | 43    | Targeted regressions for prior production-audit P1/P2 findings (notify_one on RT thread, dangling raw pointers, JSI lifetime)                                                                                                                                                                                                                                                                                                                                                                                                            |
| `test_v1_fixes`        | ASan        | 13    | v1 audit fix regressions: VP9 keyframe-request gating, factory callback wiring, single-fire transitions                                                                                                                                                                                                                                                                                                                                                                                                                                  |
| `test_transcript`      | ASan/TSan   | 13    | TranscriptRingBuffer (RT-safe push, overflow, power-of-2 invariant), TranscriptRegistry (thread-safe callbacks, multi-slot dispatch)                                                                                                                                                                                                                                                                                                                                                                                                     |
| `test_rt_safety`       | None        | 3     | Audio callback hot path: zero heap allocations, zero mutex locks, verified via malloc/pthread interposition (must build without any sanitizer)                                                                                                                                                                                                                                                                                                                                                                                           |

**Total: 368 tests** across 12 test binaries.

## Fuzz Testing

A libFuzzer harness for the WebM demuxer is available:

```bash
# With libFuzzer (requires clang with fuzzer support)
cmake -B build_fuzz -DFUZZ=ON -DSANITIZER=address
cmake --build build_fuzz -j --target fuzz_demuxer
./build_fuzz/fuzz_demuxer corpus/

# Standalone mutation mode (CI, no libFuzzer required)
cmake -B build -DSANITIZER=address
cmake --build build -j --target fuzz_demuxer
./build/fuzz_demuxer
```

## How It Works

The C++ code targets iOS/Android only (`Platform.h` gates on `TARGET_OS_IOS` / `__ANDROID__`). To compile on macOS, CMake passes `-DTARGET_OS_IOS=1 -DTARGET_OS_IPHONE=1` to fake an iOS build environment. Opus and SpeexDSP come from Homebrew rather than vendored platform libraries.
