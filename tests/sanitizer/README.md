# Sanitizer Tests (C++)

Standalone C++ tests that verify the shared `cpp/` code for memory safety and threading correctness. These run on macOS using Clang sanitizers -- no React Native or device required.

## Prerequisites

- macOS with Apple Clang (Xcode Command Line Tools)
- CMake 3.20+
- Opus library: `brew install opus`

## Running

```bash
cd tests/sanitizer

# AddressSanitizer + LeakSanitizer (memory leaks, buffer overflows, use-after-free)
mkdir -p build_asan && cd build_asan
cmake -DSANITIZER=address .. && make test_lsan && ./test_lsan

# ThreadSanitizer (data races, deadlocks)
cd ../
mkdir -p build_tsan && cd build_tsan
cmake -DSANITIZER=thread .. && make test_tsan && ./test_tsan
```

## Test coverage

| Binary      | Sanitizer | Components                                                                                                                                                                                        |
| ----------- | --------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `test_lsan` | ASAN+LSAN | FramePool, FrameQueue, PendingAudioPool, CaptureQueue, TxRingBuffer, ParticipantRegistry, SyncController, DriftCompensator, SpeexDriftResampler, CatchupController                                |
| `test_tsan` | TSAN      | All of the above under concurrent access: SPSC producer/consumer, multi-thread pool contention, AudioDecodeSlot push/read/decode, AudioMixer concurrent slot access, DecodeThread start/stop/wake |

## Known TSAN false positives

`TxRingBuffer` reports 3 data race warnings on `slot.dataSize`, `slot.timestampUs`, and `slot.seqNum` (lines 190/192/196 of `TxRingBuffer.h`). These are false positives: the slot's atomic `sequence` field with release/acquire ordering correctly synchronizes access to the non-atomic payload fields, but TSAN cannot model the sequence-number protocol's conditional happens-before edge.

## How it works

The C++ code targets iOS/Android only (`Platform.h` gates on `TARGET_OS_IOS` / `__ANDROID__`). To compile on macOS, CMake passes `-DTARGET_OS_IOS=1 -DTARGET_OS_IPHONE=1` to fake an iOS build environment. The Opus codec dependency comes from Homebrew rather than the vendored iOS XCFramework.
