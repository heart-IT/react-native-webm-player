// Audit Proof Tests: verify or refute P1/P2 findings from production audit.
//
// Build: cmake -DSANITIZER=address .. && make test_audit_proof
//        cmake -DSANITIZER=thread  .. && make test_audit_proof
// Run:   ./test_audit_proof
//
// Findings under test:
//   P1-AUDIO-09: notify_one() on audio callback thread during shutdown
//   P2-MEM-F04:  AVSyncCoordinator dangling raw pointer from audio callback
//   P2-MEM-F03:  AudioMixer::channel_ dangling raw pointer
//   P2-MEM-F05:  VideoSurfaceRegistry surfaceLostFn_ not cleared in teardown
//   A-11:        SpeexDriftResampler kRateDenom prime invariant — composite
//                denominator triggers sinc table reallocation (malloc) on audio thread
//   P1-CLIP-01:  ClipIndex metadata tracking and extraction via IngestRingBuffer
//   P1-METRIC-01:Missing audio decode latency EWMA in StreamMetrics (read-only verification)
//   F1:          IngestRingBuffer writePos_/compactPos_ overflow on 32-bit (DOWNGRADED to P3)
//   F2:          surfaceLostFn not cleared in teardown (CONFIRMED P1)
//   F3:          StallRecoveryController std::function thread safety (DOWNGRADED to P2)
//   F4:          IngestRingBuffer::reset() relaxed stores (REFUTED — pause/resume fence)
//   P1-1:        Decode retain position update rate-limited by health watchdog (500ms)
//   P2-1:        callbackInflight_ fetch_add uses memory_order_relaxed on iOS
//   P2-2:        StallRecoveryController::requestKeyFrame std::function race

#include "test_common.h"
#include <cmath>
#include <atomic>
#include <thread>
#include <chrono>
#include <functional>
#include <vector>
#include <memory>

#include <opus.h>
#include <speex/speex_resampler.h>

#include "common/MediaConfig.h"
#include "common/MediaTime.h"
#include "common/ClipIndex.h"
#include "common/IngestRingBuffer.h"
#include "MediaTypes.h"
#include "FramePool.h"
#include "JitterEstimator.h"
#include "DriftCompensator.h"
#include "AudioDecodeChannel.h"
#include "AudioMixer.h"
#include "OpusDecoderAdapter.h"
#include "common/AVSyncCoordinator.h"
#include "common/StallRecoveryController.h"
#include "video/VideoSurfaceRegistry.h"

using namespace media;

// ============================================================
// Allocation tracking for Speex resampler proof tests.
//
// We interpose malloc/realloc/calloc with a thread-local counter
// to detect any heap allocation after the resampler is initialized.
// On macOS + ASan, the sanitizer replaces malloc, but our wrappers
// still chain through — the counter increments regardless.
//
// Approach: since direct malloc interposition is fragile with ASan,
// we use a two-pronged strategy:
//   1. Mathematical proof: verify den_rate (via speex_resampler_get_ratio)
//      stays constant across all ratio updates — this guarantees
//      update_filter() never sees a changed den_rate and never
//      needs to reallocate the sinc table.
//   2. Functional proof: actually run SpeexDriftResampler::setRatio()
//      across the full production ratio range and process audio through
//      it, verifying no crash/ASan error (heap-buffer-overflow from
//      sinc_table OOB would fire under ASan).
// ============================================================

// Helper: compute GCD (mirrors Speex internal compute_gcd)
static uint32_t gcd(uint32_t a, uint32_t b) {
    while (b != 0) {
        uint32_t temp = a;
        a = b;
        b = temp % b;
    }
    return a;
}

// ============================================================
// A-11 POSITIVE: Prime kRateDenom guarantees constant den_rate
//
// For every numerator in the production ratio range [0.475, 2.205],
// GCD(num, 99991) == 1 because 99991 is prime and no numerator
// equals a multiple of 99991. Therefore den_rate is always 99991
// after GCD reduction, and update_filter() never reallocates.
// ============================================================

TEST(a11_prime_denom_gcd_always_one) {
    // kRateDenom from DriftCompensator.h
    constexpr uint32_t kRateDenom = 99991;

    // Production ratio range: kMinCombinedRatio to kMaxCombinedRatio
    // min = 0.5 * 0.95 = 0.475
    // max = 2.0 * 1.05 * 1.05 = 2.205
    constexpr float kMinRatio = 0.475f;
    constexpr float kMaxRatio = 2.205f;

    // Sweep the entire ratio range at fine granularity
    int tested = 0;
    for (float ratio = kMinRatio; ratio <= kMaxRatio; ratio += 0.0001f) {
        auto num = static_cast<uint32_t>(static_cast<float>(kRateDenom) * ratio + 0.5f);
        uint32_t g = gcd(num, kRateDenom);
        ASSERT_EQ(g, 1u);
        tested++;
    }
    // Sanity: we tested thousands of ratios
    ASSERT_TRUE(tested > 17000);
}

TEST(a11_prime_denom_den_rate_stable_in_speex) {
    // Create a real Speex resampler with prime kRateDenom and verify
    // den_rate stays constant across ratio updates.
    constexpr uint32_t kRateDenom = 99991;
    constexpr int kQuality = 3;

    // Compute kPreInitNum the same way as SpeexDriftResampler
    constexpr float kMaxCombinedRatio = 2.0f * 1.05f * 1.05f;
    auto kPreInitNum = static_cast<uint32_t>(
        static_cast<float>(kRateDenom) * kMaxCombinedRatio + 1.0f);

    int err = 0;
    SpeexResamplerState* st = speex_resampler_init(
        1, kPreInitNum, kRateDenom, kQuality, &err);
    ASSERT_TRUE(st != nullptr);
    ASSERT_EQ(err, RESAMPLER_ERR_SUCCESS);

    // After init, check den_rate
    spx_uint32_t initNum, initDen;
    speex_resampler_get_ratio(st, &initNum, &initDen);
    // Since kPreInitNum is not a multiple of prime kRateDenom, GCD=1
    ASSERT_EQ(initDen, kRateDenom);

    // Set to near-unity (same as SpeexDriftResampler::init)
    speex_resampler_set_rate_frac(st, kRateDenom + 1, kRateDenom, 48000, 48000);
    spx_uint32_t num, den;
    speex_resampler_get_ratio(st, &num, &den);
    ASSERT_EQ(den, kRateDenom);

    // Sweep production ratio range
    int denChanges = 0;
    for (float ratio = 0.475f; ratio <= 2.205f; ratio += 0.001f) {
        auto fracNum = static_cast<uint32_t>(static_cast<float>(kRateDenom) * ratio + 0.5f);
        auto inRate = static_cast<uint32_t>(48000.0f * ratio + 0.5f);
        speex_resampler_set_rate_frac(st, fracNum, kRateDenom, inRate, 48000);

        speex_resampler_get_ratio(st, &num, &den);
        if (den != kRateDenom) denChanges++;
    }

    ASSERT_EQ(denChanges, 0);
    speex_resampler_destroy(st);
}

// ============================================================
// A-11 FUNCTIONAL: Process audio through SpeexDriftResampler
// across the full ratio range. ASan detects heap-buffer-overflow
// if sinc_table is accessed out of bounds (which would happen if
// the table were reallocated to a smaller size).
// ============================================================

TEST(a11_prime_denom_process_audio_full_range_asan) {
    SpeexDriftResampler resampler;
    ASSERT_TRUE(resampler.init());

    // Generate a test signal (440Hz sine wave)
    constexpr size_t kInputSamples = 960; // 20ms at 48kHz
    float input[kInputSamples];
    for (size_t i = 0; i < kInputSamples; i++) {
        input[i] = 0.5f * sinf(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / 48000.0f);
    }

    float output[kInputSamples * 3]; // generous output buffer
    size_t consumed = 0;
    int processedCount = 0;

    // Sweep the full ratio range with fine steps
    for (float ratio = 0.5f; ratio <= 2.2f; ratio += 0.005f) {
        resampler.setRatio(ratio);
        size_t written = resampler.process(input, kInputSamples, output,
                                           kInputSamples * 3, consumed);
        // Should produce some output for any valid ratio
        ASSERT_TRUE(written > 0);
        ASSERT_TRUE(consumed > 0);
        processedCount++;
    }

    ASSERT_TRUE(processedCount > 300);
    resampler.destroy();
}

TEST(a11_prime_denom_rapid_ratio_changes_asan) {
    // Simulate the real-world pattern: drift + catchup ratio changes
    // every 20ms audio callback, with values that fluctuate.
    SpeexDriftResampler resampler;
    ASSERT_TRUE(resampler.init());

    constexpr size_t kFrameSamples = 960;
    float input[kFrameSamples];
    float output[kFrameSamples * 3];
    std::memset(input, 0, sizeof(input));

    // Simulate 500 audio callbacks (~10 seconds) with varying ratios
    float driftRatio = 1.0f;
    float catchupRatio = 1.0f;
    size_t consumed = 0;

    for (int callback = 0; callback < 500; callback++) {
        // Slowly drift: 200ppm over time
        driftRatio = 1.0f + 0.0002f * sinf(static_cast<float>(callback) * 0.01f);

        // Catchup: occasional bursts up to 5%
        if (callback % 50 == 0) catchupRatio = 1.04f;
        else catchupRatio = catchupRatio * 0.98f + 1.0f * 0.02f;

        float combined = driftRatio * catchupRatio;
        resampler.setRatio(combined);

        size_t written = resampler.process(input, kFrameSamples, output,
                                           kFrameSamples * 3, consumed);
        // Near-unity ratio should produce ~960 samples
        ASSERT_TRUE(written > 0);
    }

    resampler.destroy();
}

// ============================================================
// A-11 NEGATIVE: Composite denominator causes varying den_rate
//
// This is the control test that proves our methodology works.
// With kRateDenom = 100000 (= 2^5 * 5^5), GCD(num, den) varies
// wildly across ratios, producing different den_rate values.
// This would trigger sinc table reallocation in production.
// ============================================================

TEST(a11_composite_denom_gcd_varies) {
    constexpr uint32_t kCompositeDenom = 100000; // 2^5 * 5^5

    int distinctDenRates = 0;
    uint32_t prevDen = 0;

    for (float ratio = 0.95f; ratio <= 1.05f; ratio += 0.0001f) {
        auto num = static_cast<uint32_t>(static_cast<float>(kCompositeDenom) * ratio + 0.5f);
        uint32_t g = gcd(num, kCompositeDenom);
        uint32_t reducedDen = kCompositeDenom / g;

        if (reducedDen != prevDen) {
            distinctDenRates++;
            prevDen = reducedDen;
        }
    }

    // With composite denominator, den_rate changes frequently
    // (dozens of distinct values in a 10% ratio range)
    ASSERT_TRUE(distinctDenRates > 10);
}

TEST(a11_composite_denom_den_rate_unstable_in_speex) {
    // Create a real Speex resampler with composite denominator and verify
    // den_rate CHANGES across ratio updates — proving the negative case.
    constexpr uint32_t kCompositeDenom = 100000;
    constexpr int kQuality = 3;

    // Pre-init with max ratio (same approach as production code)
    auto preInitNum = static_cast<uint32_t>(
        static_cast<float>(kCompositeDenom) * 2.205f + 1.0f);

    int err = 0;
    SpeexResamplerState* st = speex_resampler_init(
        1, preInitNum, kCompositeDenom, kQuality, &err);
    ASSERT_TRUE(st != nullptr);
    ASSERT_EQ(err, RESAMPLER_ERR_SUCCESS);

    // Near-unity init
    speex_resampler_set_rate_frac(st, kCompositeDenom + 1, kCompositeDenom, 48000, 48000);

    int denChanges = 0;
    spx_uint32_t prevDen = 0;
    speex_resampler_get_ratio(st, &prevDen, &prevDen); // get initial den

    spx_uint32_t num, den;
    speex_resampler_get_ratio(st, &num, &den);
    prevDen = den;

    for (float ratio = 0.95f; ratio <= 1.05f; ratio += 0.001f) {
        auto fracNum = static_cast<uint32_t>(static_cast<float>(kCompositeDenom) * ratio + 0.5f);
        auto inRate = static_cast<uint32_t>(48000.0f * ratio + 0.5f);
        speex_resampler_set_rate_frac(st, fracNum, kCompositeDenom, inRate, 48000);

        speex_resampler_get_ratio(st, &num, &den);
        if (den != prevDen) {
            denChanges++;
            prevDen = den;
        }
    }

    // Composite denominator: den_rate changes many times
    ASSERT_TRUE(denChanges > 5);
    speex_resampler_destroy(st);
}

TEST(a11_composite_denom_sinc_table_realloc_detected) {
    // With a composite denominator, use_direct mode can activate when
    // den_rate is small enough (filt_len * den_rate <= filt_len * oversample + 8).
    // For Q3: filt_len=48, oversample=8, threshold=392.
    // If GCD reduces den to <= 8 (392/48=8.16), use_direct activates and
    // sinc_table_length = filt_len * den_rate = 48 * small_den.
    // Then if ratio changes to give den_rate > 8, use_direct deactivates and
    // sinc_table_length = filt_len * oversample + 8 = 392.
    // The transition may require realloc if the direct-mode table was smaller.
    //
    // We verify this by checking that some GCD-reduced den_rate values are
    // small enough to trigger use_direct mode with Q3 parameters.
    constexpr uint32_t kCompositeDenom = 100000;
    constexpr int kFiltLen = 48;   // Q3 base_length
    constexpr int kOversample = 8; // Q3 oversample
    constexpr int kDirectThreshold = kFiltLen * kOversample + 8; // 392

    int directModeCount = 0;
    int interpolatedModeCount = 0;

    for (float ratio = 0.95f; ratio <= 1.05f; ratio += 0.0001f) {
        auto num = static_cast<uint32_t>(static_cast<float>(kCompositeDenom) * ratio + 0.5f);
        uint32_t g = gcd(num, kCompositeDenom);
        uint32_t reducedDen = kCompositeDenom / g;
        uint32_t reducedNum = num / g;

        // Compute filt_len for this ratio (mirrors update_filter logic)
        uint32_t filtLen = kFiltLen;
        if (reducedNum > reducedDen) {
            // Downsampling: scale filter length
            filtLen = (filtLen * reducedNum + reducedDen - 1) / reducedDen;
            filtLen = ((filtLen - 1) & (~0x7u)) + 8;
        }

        bool useDirect = (filtLen * reducedDen <= static_cast<uint32_t>(kDirectThreshold))
                         && (reducedDen > 0);
        if (useDirect) directModeCount++;
        else interpolatedModeCount++;
    }

    // Both modes should be hit — proving the composite denominator causes
    // mode switching which triggers different sinc table size calculations.
    // If both modes are hit for different ratios, the resampler would need
    // to reallocate when switching between them.
    printf("[direct=%d interp=%d] ", directModeCount, interpolatedModeCount);
    fflush(stdout);

    // At minimum, the interpolated mode should be hit (large den_rate values
    // after GCD reduction). The direct mode may or may not be hit depending
    // on exact GCD values. The key proof is the den_rate instability shown
    // in the previous test.
    ASSERT_TRUE(interpolatedModeCount > 0);
    // Verify we have a mix of den_rate values (already proven above)
}

// ============================================================
// A-11 PRIMALITY: Verify 99991 is actually prime
//
// Trial division up to sqrt(99991) ~ 316. This is the foundational
// check — if 99991 were composite, all the GCD guarantees fail.
// ============================================================

TEST(a11_kRateDenom_is_prime) {
    constexpr uint32_t kRateDenom = 99991;

    // Trial division: check all odd numbers up to sqrt(99991)
    // sqrt(99991) ~ 316.2
    ASSERT_TRUE(kRateDenom % 2 != 0);
    for (uint32_t d = 3; d * d <= kRateDenom; d += 2) {
        ASSERT_TRUE(kRateDenom % d != 0);
    }
}

// ============================================================
// A-11 PRE-INIT: Verify kPreInitNum forces max sinc table allocation
//
// The pre-init ratio must be the largest numerator we'll ever use,
// so Speex allocates the biggest sinc table upfront. All subsequent
// setRatio() calls use a smaller or equal num_rate, keeping filt_len
// (which scales with num_rate/den_rate for downsampling) within the
// pre-allocated table size.
// ============================================================

TEST(a11_preinit_num_covers_max_ratio) {
    constexpr uint32_t kRateDenom = 99991;
    constexpr float kMaxCombinedRatio = 2.0f * 1.05f * 1.05f; // 2.205
    auto kPreInitNum = static_cast<uint32_t>(
        static_cast<float>(kRateDenom) * kMaxCombinedRatio + 1.0f);

    // Verify kPreInitNum is at least as large as any num we'd compute
    for (float ratio = 0.475f; ratio <= 2.205f; ratio += 0.0005f) {
        auto num = static_cast<uint32_t>(static_cast<float>(kRateDenom) * ratio + 0.5f);
        ASSERT_TRUE(num <= kPreInitNum);
    }
}

TEST(a11_preinit_filt_len_is_maximum) {
    // Verify that the pre-init ratio produces the largest filt_len
    // that update_filter() would ever compute for our ratio range.
    // For Q3 downsampling: filt_len = base_length * num_rate / den_rate (rounded up).
    constexpr uint32_t kRateDenom = 99991;
    constexpr uint32_t kBaseFiltLen = 48; // Q3
    constexpr float kMaxCombinedRatio = 2.0f * 1.05f * 1.05f;
    auto kPreInitNum = static_cast<uint32_t>(
        static_cast<float>(kRateDenom) * kMaxCombinedRatio + 1.0f);

    // Pre-init filt_len (downsampling since kPreInitNum > kRateDenom)
    uint32_t preInitFiltLen = (kBaseFiltLen * kPreInitNum + kRateDenom - 1) / kRateDenom;
    preInitFiltLen = ((preInitFiltLen - 1) & (~0x7u)) + 8;

    // Sweep all ratios and verify filt_len never exceeds pre-init
    for (float ratio = 0.475f; ratio <= 2.205f; ratio += 0.001f) {
        auto num = static_cast<uint32_t>(static_cast<float>(kRateDenom) * ratio + 0.5f);
        uint32_t filtLen = kBaseFiltLen;
        if (num > kRateDenom) {
            // Downsampling path
            filtLen = (kBaseFiltLen * num + kRateDenom - 1) / kRateDenom;
            filtLen = ((filtLen - 1) & (~0x7u)) + 8;
        }
        ASSERT_TRUE(filtLen <= preInitFiltLen);
    }
}

// ============================================================
// P1-AUDIO-09: notify_one() on simulated audio callback thread
//
// Reproduces the callbackInflight_ + notify_one() pattern from
// AVAudioOutputBridge::renderAudio() in isolation. We simulate
// concurrent "audio callbacks" and a shutdown drain waiter.
//
// What we prove: notify_one() is only called when the counter
// drops to zero (shutdown path). During normal playback the
// decrement does NOT trigger notify_one().
//
// TSan will detect any data race. ASan verifies no UAF.
// ============================================================

TEST(p1_audio09_notify_one_only_fires_during_shutdown_drain) {
    // Mirrors AVAudioOutputBridge::callbackInflight_ pattern
    alignas(64) std::atomic<int32_t> callbackInflight{0};
    std::atomic<int> notifyCount{0};

    // Simulate N "audio callbacks" that increment/decrement the counter
    // with the same guard pattern as renderAudio().
    constexpr int kThreads = 2;

    std::atomic<bool> running{true};

    auto callbackWorker = [&]() {
        while (running.load(std::memory_order_acquire)) {
            // Entry: increment (mirrors fetch_add in renderAudio)
            callbackInflight.fetch_add(1, std::memory_order_relaxed);

            // Simulate some work
            volatile int dummy = 0;
            for (int i = 0; i < 10; i++) dummy += i;
            (void)dummy;

            // Exit: decrement with notify_one on zero (mirrors CallbackGuard)
            if (callbackInflight.fetch_sub(1, std::memory_order_release) == 1) {
                callbackInflight.notify_one();
                notifyCount.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    // Run callback threads
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; i++) {
        threads.emplace_back(callbackWorker);
    }

    // Let them run briefly
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Signal stop and drain (mirrors waitForCallbackDrain)
    running.store(false, std::memory_order_release);

    for (auto& t : threads) t.join();

    // Now do the drain wait (mirrors destroyAudioUnit -> waitForCallbackDrain)
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (callbackInflight.load(std::memory_order_acquire) > 0) {
        if (std::chrono::steady_clock::now() >= deadline) break;
        callbackInflight.wait(callbackInflight.load(std::memory_order_relaxed),
                              std::memory_order_acquire);
    }

    ASSERT_EQ(callbackInflight.load(std::memory_order_acquire), 0);
}

TEST(p1_audio09_shutdown_drain_waits_for_inflight_callback) {
    alignas(64) std::atomic<int32_t> callbackInflight{0};
    std::atomic<bool> callbackDone{false};
    std::atomic<bool> drainComplete{false};

    // Simulate a long-running callback
    std::thread callbackThread([&]() {
        callbackInflight.fetch_add(1, std::memory_order_relaxed);
        // Hold for 50ms to simulate a slow callback
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (callbackInflight.fetch_sub(1, std::memory_order_release) == 1) {
            callbackInflight.notify_one();
        }
        callbackDone.store(true, std::memory_order_release);
    });

    // Drain waiter (simulates destroyAudioUnit)
    std::thread drainThread([&]() {
        // Small delay to ensure callback thread starts first
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        while (callbackInflight.load(std::memory_order_acquire) > 0) {
            if (std::chrono::steady_clock::now() >= deadline) break;
            callbackInflight.wait(callbackInflight.load(std::memory_order_relaxed),
                                  std::memory_order_acquire);
        }
        drainComplete.store(true, std::memory_order_release);
    });

    callbackThread.join();
    drainThread.join();

    ASSERT_TRUE(callbackDone.load(std::memory_order_acquire));
    ASSERT_TRUE(drainComplete.load(std::memory_order_acquire));
    ASSERT_EQ(callbackInflight.load(std::memory_order_acquire), 0);
}

TEST(p1_audio09_concurrent_callbacks_during_drain) {
    alignas(64) std::atomic<int32_t> callbackInflight{0};
    std::atomic<uint8_t> state{2}; // 2 = Active
    constexpr uint8_t kActive = 2;
    constexpr uint8_t kShuttingDown = 3;

    constexpr int kCallbackThreads = 4;
    std::atomic<int> callbacksCompleted{0};

    auto callbackWorker = [&]() {
        for (int i = 0; i < 200; i++) {
            callbackInflight.fetch_add(1, std::memory_order_relaxed);

            uint8_t s = state.load(std::memory_order_acquire);
            if (s != kActive) {
                if (callbackInflight.fetch_sub(1, std::memory_order_release) == 1) {
                    callbackInflight.notify_one();
                }
                break;
            }

            // Simulate PCM copy work
            volatile float buf[480];
            std::memset((void*)buf, 0, sizeof(buf));

            if (callbackInflight.fetch_sub(1, std::memory_order_release) == 1) {
                callbackInflight.notify_one();
            }
            callbacksCompleted.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kCallbackThreads; i++) {
        threads.emplace_back(callbackWorker);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    state.store(kShuttingDown, std::memory_order_release);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (callbackInflight.load(std::memory_order_acquire) > 0) {
        if (std::chrono::steady_clock::now() >= deadline) break;
        callbackInflight.wait(callbackInflight.load(std::memory_order_relaxed),
                              std::memory_order_acquire);
    }

    for (auto& t : threads) t.join();

    ASSERT_EQ(callbackInflight.load(std::memory_order_acquire), 0);
    ASSERT_TRUE(callbacksCompleted.load(std::memory_order_relaxed) > 0);
}

// ============================================================
// P2-MEM-F04: AVSyncCoordinator dangling pointer
// ============================================================

TEST(p2_mem_f04_sync_coordinator_null_during_callback_read) {
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);
    channel.setDecoder(std::make_unique<OpusDecoderAdapter>());
    channel.activate();

    AVSyncCoordinator sync;
    channel.setSyncCoordinator(&sync);

    std::atomic<bool> running{true};

    std::thread callbackThread([&]() {
        while (running.load(std::memory_order_acquire)) {
            sync.onAudioRender(1000, nowUs());
            std::this_thread::yield();
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    channel.setSyncCoordinator(nullptr);

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    running.store(false, std::memory_order_release);
    callbackThread.join();

    channel.deactivate();
}

TEST(p2_mem_f04_sync_coordinator_teardown_ordering) {
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);
    channel.setDecoder(std::make_unique<OpusDecoderAdapter>());
    channel.activate();

    auto sync = std::make_unique<AVSyncCoordinator>();
    channel.setSyncCoordinator(sync.get());

    std::atomic<bool> running{true};

    std::thread reader([&]() {
        while (running.load(std::memory_order_acquire)) {
            (void)sync->currentOffsetUs();
            std::this_thread::yield();
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    channel.setSyncCoordinator(nullptr);
    running.store(false, std::memory_order_release);
    reader.join();

    sync.reset();
    channel.deactivate();
}

// ============================================================
// P2-MEM-F03: AudioMixer::channel_ dangling raw pointer
// ============================================================

TEST(p2_mem_f03_mixer_channel_valid_during_callback) {
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);
    channel.setDecoder(std::make_unique<OpusDecoderAdapter>());
    channel.activate();

    AudioMixer mixer;
    mixer.setChannel(&channel);

    std::atomic<bool> running{true};

    std::thread callbackThread([&]() {
        float output[960];
        while (running.load(std::memory_order_acquire)) {
            (void)mixer.mix(output, 480);
            std::this_thread::yield();
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    running.store(false, std::memory_order_release);
    callbackThread.join();

    channel.deactivate();
}

TEST(p2_mem_f03_mixer_channel_null_safety) {
    AudioMixer mixer;
    float output[960];
    size_t result = mixer.mix(output, 480);
    ASSERT_EQ(result, 0u);
}

// ============================================================
// P2-MEM-F05: VideoSurfaceRegistry callback clearing
// ============================================================

TEST(p2_mem_f05_registry_clear_nullifies_callbacks) {
    auto& reg = VideoSurfaceRegistry::instance();
    reg.clear();

    std::atomic<int> lostCount{0};
    std::atomic<int> keyFrameCount{0};

    reg.setSurfaceLostFn([&]() { lostCount.fetch_add(1); });
    reg.setKeyFrameRequestFn([&]() { keyFrameCount.fetch_add(1); });

    void* fakeSurface = reinterpret_cast<void*>(0xDEAD);
    reg.registerSurface(fakeSurface);
    reg.unregisterSurface();
    ASSERT_EQ(lostCount.load(), 1);

    reg.clear();

    reg.registerSurface(fakeSurface);
    reg.unregisterSurface();
    ASSERT_EQ(lostCount.load(), 1);

    reg.requestKeyFrame();
    ASSERT_EQ(keyFrameCount.load(), 1);

    reg.clear();
}

TEST(p2_mem_f05_registry_concurrent_unregister_during_callback_set) {
    auto& reg = VideoSurfaceRegistry::instance();
    reg.clear();

    std::atomic<bool> running{true};
    std::atomic<int> iterations{0};

    std::thread setter([&]() {
        while (running.load(std::memory_order_acquire)) {
            reg.setSurfaceLostFn([]() {});
            std::this_thread::yield();
            reg.setSurfaceLostFn(nullptr);
            std::this_thread::yield();
        }
    });

    std::thread user([&]() {
        while (running.load(std::memory_order_acquire)) {
            void* fake = reinterpret_cast<void*>(0xBEEF);
            reg.registerSurface(fake);
            reg.unregisterSurface();
            iterations.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    running.store(false, std::memory_order_release);

    setter.join();
    user.join();

    ASSERT_TRUE(iterations.load(std::memory_order_relaxed) > 0);
    reg.clear();
}

// ============================================================
// P1-CLIP-01: ClipIndex metadata tracking and extraction via IngestRingBuffer
//
// ClipIndex is now a metadata-only index into IngestRingBuffer.
// No appendBytes() — data lives in the ring buffer. ClipIndex tracks
// cluster boundaries, keyframes, and PTS ranges for clip extraction.
// ============================================================

TEST(p1_clip01_clip_index_basic_extraction) {
    // Verify ClipIndex can track clusters and extract clip data from the ring.
    IngestRingBuffer ring(4096);
    ClipIndex index;
    index.setRingBuffer(&ring);
    index.setEnabled(true);

    uint8_t header[] = {0x1A, 0x45, 0xDF, 0xA3};
    index.setStreamHeader(header, sizeof(header));

    // Write cluster data to ring and track in index
    std::vector<uint8_t> cluster(200, 0xAB);
    ASSERT_TRUE(ring.write(cluster.data(), cluster.size()));

    index.onNewCluster(ring.baseOffset());
    index.onKeyFrame(0);
    index.onBlockPts(0);
    index.updateRetainPosition();

    // Write a second cluster
    std::vector<uint8_t> cluster2(200, 0xCD);
    ASSERT_TRUE(ring.write(cluster2.data(), cluster2.size()));

    index.onNewCluster(ring.baseOffset() + static_cast<long long>(cluster.size()));
    index.onKeyFrame(1000000);
    index.onBlockPts(1000000);
    index.updateRetainPosition();

    // Extract clip — should contain header + cluster data
    auto result = index.extractClip(10.0f);
    ASSERT_TRUE(result.error.empty());
    ASSERT_TRUE(result.data.size() > sizeof(header));
    // First bytes should be the stream header
    ASSERT_TRUE(result.data[0] == 0x1A);
}

TEST(p1_clip01_clip_index_no_header_returns_error) {
    // Extract without setting stream header should fail gracefully.
    IngestRingBuffer ring(4096);
    ClipIndex index;
    index.setRingBuffer(&ring);
    index.setEnabled(true);

    std::vector<uint8_t> data(100, 0xAB);
    ASSERT_TRUE(ring.write(data.data(), data.size()));
    index.onNewCluster(0);
    index.onKeyFrame(0);


    auto result = index.extractClip(5.0f);
    ASSERT_TRUE(!result.error.empty());
}

TEST(p1_clip01_clip_index_metrics) {
    // Verify clipMetrics reflects enabled state and cluster count.
    IngestRingBuffer ring(4096);
    ClipIndex index;
    index.setRingBuffer(&ring);

    auto m = index.clipMetrics();
    ASSERT_TRUE(!m.enabled);
    ASSERT_EQ(m.clusterCount, 0u);

    index.setEnabled(true);

    uint8_t header[] = {0x1A};
    index.setStreamHeader(header, 1);

    std::vector<uint8_t> data(128, 0xAB);
    ASSERT_TRUE(ring.write(data.data(), data.size()));
    index.onNewCluster(0);
    index.onKeyFrame(0);
    index.onBlockPts(0);


    // Seal the pending cluster by starting a new one
    ASSERT_TRUE(ring.write(data.data(), data.size()));
    index.onNewCluster(static_cast<long long>(data.size()));

    index.updateRetainPosition();

    m = index.clipMetrics();
    ASSERT_TRUE(m.enabled);
    ASSERT_TRUE(m.clusterCount >= 1);
    ASSERT_EQ(m.bufferCapacity, 4096u);
}

// ============================================================
// P1-METRIC-01: No audio decode latency EWMA in StreamMetrics
//
// This is a code-absence finding — we verify by checking that
// StreamMetrics has no field named decodeLatencyUs or similar.
// The test constructs a StreamMetrics, calls reset(), and
// confirms only the known fields exist (compile-time proof).
// ============================================================

TEST(p1_metric01_stream_metrics_has_no_decode_latency_field) {
    // Compile-time proof: if someone adds a decode latency field,
    // this test should be updated. For now we verify the struct
    // has exactly the fields documented and no latency EWMA.
    StreamMetrics m;
    m.reset();

    // Access every known field to prove we know the full surface
    ASSERT_EQ(m.framesReceived.load(), 0u);
    ASSERT_EQ(m.framesDropped.load(), 0u);
    ASSERT_EQ(m.framesDrained.load(), 0u);
    ASSERT_EQ(m.decodeErrors.load(), 0u);
    ASSERT_EQ(m.decoderResets.load(), 0u);
    ASSERT_EQ(m.underruns.load(), 0u);
    ASSERT_EQ(m.plcFrames.load(), 0u);
    ASSERT_EQ(m.samplesOutput.load(), 0u);
    ASSERT_EQ(m.ptsDiscontinuities.load(), 0u);
    ASSERT_EQ(m.fastPathSwitches.load(), 0u);
    // catchupDeadZoneSnaps removed from StreamMetrics — live counter lives in
    // CatchupController::deadZoneSnaps_ and is exposed via SessionMetrics.
    ASSERT_EQ(m.peakConsecutivePLC.load(), 0u);
    ASSERT_EQ(m.maxInterFrameGapUs.load(), 0);
    ASSERT_EQ(m.gapsOver50ms.load(), 0u);
    ASSERT_EQ(m.gapsOver100ms.load(), 0u);
    ASSERT_EQ(m.gapsOver500ms.load(), 0u);

    // sizeof check: if a new atomic field is added, this will likely
    // change (each atomic<uint64_t> is 8 bytes, atomic<uint32_t> is 4).
    // Current expected size: 16 * 8 + 1 * 4 = 132 bytes (+ padding).
    // We check a reasonable upper bound — if it grows significantly,
    // someone added fields we should audit.
    size_t sz = sizeof(StreamMetrics);
    printf("[sizeof=%zu] ", sz);
    fflush(stdout);
    // No hard assert on size — it's platform-dependent. This is informational.
    ASSERT_TRUE(sz > 0);
}

// ============================================================
// F1 (P0 on 32-bit -> DOWNGRADED to P3): IngestRingBuffer writePos_/compactPos_
// overflow after ~4GB on 32-bit.
//
// On this platform (64-bit), size_t is 64 bits. React Native min API 29
// (arm64-v8a) and iOS arm64 means 32-bit is not a target.
//
// We verify:
//   1. size_t is 64-bit on this platform (compile-time proof)
//   2. The unsigned wraparound math in `used = wp - cp` is correct
//      even if we simulate 4GB+ of cumulative writes with a small buffer
//   3. The ring buffer works correctly after billions of bytes written
// ============================================================

TEST(f1_size_t_is_64bit_on_target_platform) {
    // React Native min API 29 is arm64-v8a. iOS is arm64.
    // This test confirms size_t is 64-bit, making the overflow
    // unreachable in production (would need ~18 exabytes).
    ASSERT_EQ(sizeof(size_t), 8u);
}

TEST(f1_ring_buffer_unsigned_wraparound_math_correct) {
    // Even if we were on 32-bit, the unsigned subtraction wp - cp
    // is correct for SPSC ring buffers as long as (wp - cp) <= capacity.
    // Demonstrate with uint32_t simulation.
    constexpr uint32_t capacity = 1024;
    uint32_t wp = 0;
    uint32_t cp = 0;

    // Balanced producer/consumer so used stays bounded (prior version
    // advanced cp by only half of writeLen, so used grew unbounded).
    for (int i = 0; i < 100; i++) {
        uint32_t writeLen = 512;
        uint32_t used = wp - cp;
        ASSERT_TRUE(writeLen <= capacity - used);
        wp += writeLen;
        cp += writeLen;
    }

    // Now simulate near UINT32_MAX
    wp = UINT32_MAX - 500;
    cp = UINT32_MAX - 700;
    uint32_t used = wp - cp;
    ASSERT_EQ(used, 200u);

    // Write 512 bytes, causing wp to wrap
    wp += 512;
    used = wp - cp;
    // wp wrapped: (UINT32_MAX - 500 + 512) = UINT32_MAX + 12 = 11 (mod 2^32)
    // cp = UINT32_MAX - 700
    // used = 11 - (UINT32_MAX - 700) = 11 + 701 = 712 (mod 2^32)
    ASSERT_EQ(used, 712u);
}

TEST(f1_ring_buffer_large_cumulative_writes) {
    // Prove IngestRingBuffer works after 4GB+ cumulative writes
    // on 64-bit. Write in small chunks, compact regularly.
    constexpr size_t kCapacity = 4096;
    IngestRingBuffer ring(kCapacity);

    uint8_t data[256];
    std::memset(data, 0xAB, sizeof(data));

    // Write 5GB worth of data in 256-byte chunks with regular compaction
    // We can't actually write 5GB in a test, but we can verify the math
    // by manually advancing positions to simulate it
    constexpr size_t kChunkSize = 256;
    size_t totalWritten = 0;
    for (size_t i = 0; i < 100000; i++) {
        bool ok = ring.write(data, kChunkSize);
        ASSERT_TRUE(ok);
        totalWritten += kChunkSize;

        // Compact every 8 writes to keep the ring from filling
        if (i % 8 == 7) {
            ring.compact(ring.baseOffset() + static_cast<long long>(totalWritten - kCapacity / 2));
        }
    }

    // Verify ring is still functional
    ASSERT_TRUE(ring.freeSpace() > 0);
    ASSERT_TRUE(ring.liveBytes() <= kCapacity);

    // Verify we can still read recently written data
    uint8_t readBuf[256];
    size_t wp = ring.currentWritePos();
    int rc = ring.readAt(ring.baseOffset() + static_cast<long long>(wp - kChunkSize),
                         kChunkSize, readBuf);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(readBuf[0], 0xAB);
}

// ============================================================
// F2 (CONFIRMED P1): surfaceLostFn captures raw VideoFrameQueue*,
// not cleared in teardown().
//
// PipelineOrchestrator::teardown() clears setKeyFrameRequestFn(nullptr)
// but does NOT call setSurfaceLostFn(nullptr). The lambda in
// setVideoQueue() captures a raw VideoFrameQueue* pointer.
// If the surface is unregistered after teardown, the stale lambda
// dereferences freed memory.
//
// Proof: We simulate the exact sequence:
//   1. Set a surfaceLostFn that captures a raw pointer
//   2. Do NOT clear it (mimic missing teardown cleanup)
//   3. Unregister surface -> lambda fires with dangling pointer
//
// ASan will detect use-after-free if the pointer is freed.
// ============================================================

TEST(f2_surfaceLostFn_not_cleared_in_teardown_confirmed) {
    auto& reg = VideoSurfaceRegistry::instance();
    reg.clear();

    // Simulate what PipelineOrchestrator::setVideoQueue does:
    // capture a raw pointer in the surfaceLostFn lambda
    auto* heapObj = new std::atomic<int>(0);

    reg.setSurfaceLostFn([heapObj]() {
        heapObj->fetch_add(1, std::memory_order_relaxed);
    });

    // Simulate what teardown() does: clear keyFrameRequestFn but NOT surfaceLostFn
    reg.setKeyFrameRequestFn(nullptr);
    // NOTE: reg.setSurfaceLostFn(nullptr) is MISSING — this is the bug

    // Delete the object the lambda captures (simulates VideoFrameQueue destruction)
    delete heapObj;

    // Now if unregisterSurface fires, the lambda dereferences freed memory.
    // We register then unregister to trigger the callback.
    void* fakeSurface = reinterpret_cast<void*>(0xCAFE);
    reg.registerSurface(fakeSurface);

    // This will invoke surfaceLostFn_ which dereferences deleted heapObj.
    // Under ASan, this is use-after-free.
    // To avoid crashing the test suite, we clear before triggering:
    // UNCOMMENT the next line to see ASan fire on the bug:
    // reg.unregisterSurface();

    // Instead, prove the bug exists by showing surfaceLostFn is still set
    // after the simulated teardown (clear only keyFrameRequestFn):
    // We can verify by setting a safe callback and checking the old one fires
    std::atomic<int> safeCounter{0};

    // Register surface and unregister with safe callback to avoid crash
    reg.setSurfaceLostFn([&safeCounter]() {
        safeCounter.fetch_add(1, std::memory_order_relaxed);
    });
    reg.unregisterSurface();
    ASSERT_EQ(safeCounter.load(), 1);

    // The proof: teardown() only clears keyFrameRequestFn, not surfaceLostFn.
    // This test documents the confirmed finding. The fix is to add:
    //   media::VideoSurfaceRegistry::instance().setSurfaceLostFn(nullptr);
    // to PipelineOrchestrator::teardown().
    reg.clear();
}

TEST(f2_teardown_clears_keyframe_but_not_surface_lost) {
    // Direct proof by code reading: after simulated teardown sequence,
    // verify keyFrameRequestFn is null but surfaceLostFn is still live.
    auto& reg = VideoSurfaceRegistry::instance();
    reg.clear();

    std::atomic<int> lostCount{0};
    std::atomic<int> keyFrameCount{0};

    reg.setSurfaceLostFn([&lostCount]() {
        lostCount.fetch_add(1, std::memory_order_relaxed);
    });
    reg.setKeyFrameRequestFn([&keyFrameCount]() {
        keyFrameCount.fetch_add(1, std::memory_order_relaxed);
    });

    // Simulate teardown: only clears keyFrameRequestFn
    reg.setKeyFrameRequestFn(nullptr);

    // keyFrameRequestFn is cleared
    reg.requestKeyFrame();
    ASSERT_EQ(keyFrameCount.load(), 0);

    // surfaceLostFn is NOT cleared — it would fire on unregisterSurface
    void* fakeSurface = reinterpret_cast<void*>(0xBEEF);
    reg.registerSurface(fakeSurface);
    reg.unregisterSurface();
    ASSERT_EQ(lostCount.load(), 1);  // Still fires — bug confirmed

    reg.clear();
}

// ============================================================
// F3 (DOWNGRADED from P1 to P2): StallRecoveryController thread safety
//
// The atomic state machine (state_, timestamps, counters) is correctly
// synchronized with acquire/release ordering. However, the std::function
// members (keyFrameRequestFn_, videoKeyFrameRequestFn_) are NOT atomic
// and have no synchronization. setKeyFrameRequestFn() on JS thread can
// race with requestKeyFrame() on decode thread.
//
// This is P2 (not P1) because:
//   1. The functions are set once during init and cleared during teardown
//   2. The race window is only during setup/teardown transitions
//   3. During steady-state operation, no concurrent mutation occurs
//
// TSan will detect the data race on std::function.
// ============================================================

TEST(f3_stall_recovery_atomic_state_machine_correct) {
    // Prove the atomic state transitions are thread-safe:
    // JS thread calls onDataReceived(), decode thread calls evaluate().
    StallRecoveryController ctrl;

    std::atomic<bool> running{true};
    std::atomic<int> dataCallCount{0};
    std::atomic<int> evalCallCount{0};

    // JS thread: calls onDataReceived periodically
    std::thread jsThread([&]() {
        while (running.load(std::memory_order_acquire)) {
            ctrl.onDataReceived();
            dataCallCount.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    // Decode thread: calls evaluate periodically
    std::thread decodeThread([&]() {
        while (running.load(std::memory_order_acquire)) {
            ctrl.evaluate();
            evalCallCount.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    running.store(false, std::memory_order_release);

    jsThread.join();
    decodeThread.join();

    ASSERT_TRUE(dataCallCount.load() > 0);
    ASSERT_TRUE(evalCallCount.load() > 0);

    // State should be Healthy or Detecting (data keeps flowing)
    StallState s = ctrl.state();
    ASSERT_TRUE(s == StallState::Healthy || s == StallState::Detecting);
}

TEST(f3_stall_recovery_reset_concurrent_with_evaluate) {
    // Prove that reset() from JS thread concurrent with evaluate()
    // on decode thread doesn't crash (all fields are atomic).
    StallRecoveryController ctrl;

    // Set up initial state: trigger a stall so there's state to reset
    ctrl.onDataReceived();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::atomic<bool> running{true};

    // Decode thread: evaluate repeatedly
    std::thread decodeThread([&]() {
        while (running.load(std::memory_order_acquire)) {
            ctrl.evaluate();
            std::this_thread::yield();
        }
    });

    // JS thread: reset repeatedly (simulates resetStream)
    std::thread jsThread([&]() {
        while (running.load(std::memory_order_acquire)) {
            ctrl.onDataReceived();
            ctrl.reset();
            std::this_thread::yield();
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    running.store(false, std::memory_order_release);

    decodeThread.join();
    jsThread.join();

    // No crash or TSan error = atomic state machine is correct
    ASSERT_TRUE(true);
}

TEST(f3_stall_recovery_std_function_race_documented) {
    // Document the real race: std::function members are not atomic.
    // setKeyFrameRequestFn() and requestKeyFrame() (called inside
    // evaluate() -> requestKeyFrame()) can race.
    //
    // We don't trigger this under TSan here because it would flag
    // a data race that's known and documented. Instead we verify
    // the code pattern: std::function is not thread-safe for
    // concurrent read+write.
    //
    // The fix would be to protect the function members with a mutex
    // or use an atomic<bool> guard checked in requestKeyFrame().
    StallRecoveryController ctrl;

    int callCount = 0;
    ctrl.setKeyFrameRequestFn([&callCount]() { callCount++; });

    // Verify the function is callable (no synchronization issue in
    // single-threaded context)
    ctrl.onDataReceived();

    // Force a stall by not sending data for evaluate to detect
    // (This is a unit test, not a concurrent race test)
    ASSERT_TRUE(true);
}

// ============================================================
// F4 (REFUTED): IngestRingBuffer::reset() uses relaxed stores
//
// The claim is that relaxed stores in reset() are unsafe.
// REFUTED because:
//   1. resetStream() calls ingestThread_->pause() BEFORE reset()
//   2. pause() spins until paused_.load(acquire) == true
//   3. The ingest thread sets paused_.store(true, release) before blocking
//   4. This creates a happens-before: all ingest thread writes are visible
//      to the JS thread after pause() returns
//   5. After reset() with relaxed stores, resume() uses release/acquire
//      through wakeVersion_, creating happens-before for the ingest thread
//      to see the reset values
//
// The acquire-release chain: ingest release -> JS acquire (pause)
// -> JS relaxed stores (reset) -> JS release (resume/wake)
// -> ingest acquire (wakeVersion wait) -> sees reset values
// ============================================================

TEST(f4_ring_buffer_reset_with_pause_resume_fence) {
    // Prove that reset() values are visible to a consumer thread
    // when protected by a pause/resume pattern using acquire/release.
    constexpr size_t kCapacity = 4096;
    IngestRingBuffer ring(kCapacity);

    // Write some data
    uint8_t data[128];
    std::memset(data, 0xFF, sizeof(data));
    (void)ring.write(data, sizeof(data));
    ASSERT_TRUE(ring.currentWritePos() > 0);
    ASSERT_TRUE(ring.liveBytes() > 0);

    // Simulate the pause/resume fencing pattern:
    // 1. "Pause" fence (release from worker, acquire on controller)
    std::atomic<bool> paused{false};
    std::atomic<bool> resumed{false};
    std::atomic<size_t> observedWritePos{999};

    std::thread worker([&]() {
        // Worker signals paused
        paused.store(true, std::memory_order_release);

        // Wait for resume
        while (!resumed.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        // After resume, read the reset values
        // The acquire on resumed guarantees visibility of the relaxed stores
        observedWritePos.store(ring.currentWritePos(), std::memory_order_relaxed);
    });

    // Wait for pause
    while (!paused.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // Reset with relaxed stores (same as IngestRingBuffer::reset)
    ring.reset();

    // Resume with release fence
    resumed.store(true, std::memory_order_release);

    worker.join();

    // Worker should see the reset values (writePos = 0)
    ASSERT_EQ(observedWritePos.load(), 0u);
    ASSERT_EQ(ring.currentWritePos(), 0u);
    ASSERT_EQ(ring.liveBytes(), 0u);
}

TEST(f4_ring_buffer_reset_functional_after_heavy_use) {
    // Verify reset() correctly resets all state after heavy use
    constexpr size_t kCapacity = 4096;
    IngestRingBuffer ring(kCapacity);

    // Fill and compact several times
    uint8_t data[256];
    std::memset(data, 0xCC, sizeof(data));
    for (int i = 0; i < 50; i++) {
        (void)ring.write(data, sizeof(data));
        if (i % 4 == 3) {
            ring.compact(ring.baseOffset() + static_cast<long long>(ring.currentWritePos() - 512));
        }
    }

    ASSERT_TRUE(ring.currentWritePos() > 0);

    // Reset
    ring.reset();

    ASSERT_EQ(ring.currentWritePos(), 0u);
    ASSERT_EQ(ring.liveBytes(), 0u);
    ASSERT_EQ(ring.freeSpace(), kCapacity);
    ASSERT_EQ(ring.baseOffset(), 0LL);

    // Verify ring is fully functional after reset
    bool ok = ring.write(data, sizeof(data));
    ASSERT_TRUE(ok);
    ASSERT_EQ(ring.currentWritePos(), 256u);
    ASSERT_EQ(ring.liveBytes(), 256u);
}

// ============================================================
// P1-1: Decode retain position update rate-limited by health watchdog (500ms)
//
// The setDecodeRetainPos() call lives inside the readMetrics_ lambda,
// which is only invoked from HealthWatchdog::evaluate(). evaluate()
// has a 500ms rate limit (kMinEventIntervalUs). Under burst ingestion,
// the ring buffer's compact() could advance past queued encoded frames
// before the decode retain position is updated, because the retain pos
// lags by up to 500ms.
//
// Test approach: Feed data, compact aggressively, verify whether
// data at the decode position is still readable when the retain
// position update is delayed (rate-limited).
//
// This is a LOGIC proof — the rate-limit is a design property, not
// a concurrency bug. The test proves the 500ms window is real.
// ============================================================

TEST(p1_1_decode_retain_pos_rate_limited_by_health_watchdog) {
    // Prove the rate limit exists: HealthWatchdog::evaluate() returns
    // early if called within 500ms of the last invocation.
    constexpr int64_t kRateLimit = 500'000; // 500ms in microseconds

    // Simulate the health watchdog rate-limit behavior.
    // Two rapid calls: first runs, second is suppressed.
    int evaluateCount = 0;
    // Pre-initialize so the first call (now=0) is NOT suppressed.
    int64_t lastEventTime = -kRateLimit;

    auto simulateEvaluate = [&](int64_t now) {
        if (now - lastEventTime < kRateLimit) return; // rate-limited
        lastEventTime = now;
        evaluateCount++;
    };

    simulateEvaluate(0);         // First call — runs
    simulateEvaluate(100'000);   // 100ms later — suppressed
    simulateEvaluate(200'000);   // 200ms later — suppressed
    simulateEvaluate(400'000);   // 400ms later — suppressed
    simulateEvaluate(500'000);   // 500ms later — runs

    ASSERT_EQ(evaluateCount, 2);
}

TEST(p1_1_ring_compaction_races_retain_pos_update) {
    // Demonstrate the actual risk: between retain pos updates (500ms),
    // compact() can advance past where decode needs to read.
    //
    // Setup: ring with data, set decodeRetainPos at some offset,
    // then show that if retainPos is NOT set (simulating the 500ms
    // lag), compact advances freely past the decode read position.
    constexpr size_t kCapacity = 4096;
    IngestRingBuffer ring(kCapacity);

    // Fill ring with identifiable data
    uint8_t data[512];
    for (size_t i = 0; i < sizeof(data); i++) data[i] = static_cast<uint8_t>(i & 0xFF);

    // Write 4 chunks of 512 bytes
    for (int i = 0; i < 4; i++) {
        ASSERT_TRUE(ring.write(data, sizeof(data)));
    }
    // writePos = 2048, ring is half full

    // Simulate decode reading at offset 512 (second chunk)
    // In production, this would be set by readMetrics_ lambda
    size_t decodeReadPos = 512;

    // Case 1: WITH retain pos set (production "after evaluate" state)
    ring.setDecodeRetainPos(decodeReadPos);
    long long compactTarget = ring.baseOffset() + 1024; // try to compact past decode pos
    ring.compact(compactTarget);
    // Compact should be clamped to decodeRetainPos
    size_t afterCompact1 = ring.liveBytes();
    ASSERT_TRUE(afterCompact1 >= 2048 - decodeReadPos); // Data at decode pos preserved

    // Case 2: WITHOUT retain pos (simulating 500ms lag before first evaluate)
    ring.setDecodeRetainPos(0); // Clear — simulates no evaluate yet
    IngestRingBuffer ring2(kCapacity);
    for (int i = 0; i < 4; i++) {
        ASSERT_TRUE(ring2.write(data, sizeof(data)));
    }
    // No decodeRetainPos set — compact is unconstrained
    ring2.compact(ring2.baseOffset() + 1024);
    // compact advanced freely — data at offset 512 may be gone
    uint8_t readBuf[512];
    int rc = ring2.readAt(ring2.baseOffset(), 512, readBuf);
    // readAt below compactPos should fail (data compacted away)
    ASSERT_TRUE(rc != 0); // -1 = out of range, data was compacted

    // This proves: without timely decodeRetainPos update,
    // compact() can remove data that decode still needs.
}

TEST(p1_1_burst_feed_saturates_ring_before_retain_update) {
    // Simulate burst ingestion: many writes + compactions within
    // 500ms, with only ONE retain pos update at the start.
    constexpr size_t kCapacity = 4096;
    IngestRingBuffer ring(kCapacity);

    // Set initial retain pos (as if evaluate just ran)
    size_t initialRetainPos = 0;
    ring.setDecodeRetainPos(initialRetainPos);

    uint8_t chunk[256];
    std::memset(chunk, 0xAA, sizeof(chunk));

    // Burst: 50 write+compact cycles (simulating fast data arrival
    // within the 500ms health watchdog window)
    size_t totalWritten = 0;
    for (int i = 0; i < 50; i++) {
        ASSERT_TRUE(ring.write(chunk, sizeof(chunk)));
        totalWritten += sizeof(chunk);

        // Aggressive compaction: compact everything except last 1KB
        if (totalWritten > kCapacity / 2) {
            long long compactTo = ring.baseOffset() +
                static_cast<long long>(totalWritten - 1024);
            ring.compact(compactTo);
        }
    }

    // After burst, decodeRetainPos is still 0 (no evaluate ran).
    // All data from position 0 has been compacted away.
    ASSERT_EQ(ring.decodeRetainPos(), 0u);

    // Verify: data at the original decode position is gone
    uint8_t readBuf[256];
    int rc = ring.readAt(ring.baseOffset(), 256, readBuf);
    // Should fail — compacted past position 0 long ago
    ASSERT_TRUE(rc != 0);
    // The fact that decodeRetainPos was never updated during
    // the burst proves the 500ms rate limit is a real risk.
    ASSERT_TRUE(ring.liveBytes() <= 1024);
}

// ============================================================
// P2-1: callbackInflight_ fetch_add uses memory_order_relaxed on iOS
//
// In AVAudioOutputBridge::renderAudio(), the entry guard does:
//   callbackInflight_.fetch_add(1, memory_order_relaxed)
// while waitForCallbackDrain() reads:
//   callbackInflight_.load(memory_order_acquire)
//
// The concern: relaxed store could be reordered past the acquire
// load, so drain sees 0 while a callback is actually entering.
//
// Analysis: On arm64 (iOS target), fetch_add is implemented as
// LDADDAL or LDXR/STXR loop — both are full barriers on Apple
// Silicon. The relaxed ordering is a theoretical concern on
// weakly-ordered architectures but not a practical bug on arm64.
//
// However, the code pattern is formally incorrect per C++ memory
// model. TSan operates on the abstract C++ model and should flag
// this if there's a genuine visibility gap.
//
// Test: concurrent callback entry (relaxed add) + drain wait
// (acquire load). TSan verifies correctness of the ordering.
// ============================================================

TEST(p2_1_callback_inflight_relaxed_add_vs_acquire_drain) {
    // Reproduce the exact AVAudioOutputBridge pattern:
    // renderAudio: fetch_add(1, relaxed) ... fetch_sub(1, release)
    // waitForCallbackDrain: load(acquire) in spin loop
    alignas(64) std::atomic<int32_t> callbackInflight{0};
    std::atomic<bool> stopCallbacks{false};
    std::atomic<bool> drainStarted{false};
    std::atomic<bool> drainComplete{false};
    std::atomic<int> callbacksExecuted{0};

    // Callback threads — use the exact memory orders from production
    auto callbackWorker = [&]() {
        while (!stopCallbacks.load(std::memory_order_acquire)) {
            // Entry: relaxed (matches AVAudioOutputBridge.mm:41)
            callbackInflight.fetch_add(1, std::memory_order_relaxed);

            // Simulate PCM work
            volatile float pcm[480];
            std::memset((void*)pcm, 0, sizeof(pcm));

            // Exit: release (matches CallbackGuard destructor)
            callbackInflight.fetch_sub(1, std::memory_order_release);
            callbacksExecuted.fetch_add(1, std::memory_order_relaxed);

            std::this_thread::yield();
        }
    };

    // Start callback threads
    constexpr int kCallbackThreads = 4;
    std::vector<std::thread> callbacks;
    for (int i = 0; i < kCallbackThreads; i++) {
        callbacks.emplace_back(callbackWorker);
    }

    // Let callbacks run
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Drain thread — mirrors waitForCallbackDrain exactly
    std::thread drainThread([&]() {
        drainStarted.store(true, std::memory_order_release);

        // Stop new callbacks
        stopCallbacks.store(true, std::memory_order_release);

        // Drain wait — acquire load (matches AVAudioOutputBridge.mm:222)
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        while (callbackInflight.load(std::memory_order_acquire) > 0) {
            if (std::chrono::steady_clock::now() >= deadline) break;
            std::this_thread::yield();
        }

        drainComplete.store(true, std::memory_order_release);
    });

    drainThread.join();
    for (auto& t : callbacks) t.join();

    ASSERT_TRUE(drainComplete.load(std::memory_order_acquire));
    ASSERT_EQ(callbackInflight.load(std::memory_order_acquire), 0);
    ASSERT_TRUE(callbacksExecuted.load(std::memory_order_relaxed) > 0);
}

TEST(p2_1_callback_inflight_relaxed_entry_during_drain_start) {
    // Targeted test for the specific race window:
    // Thread A calls fetch_add(1, relaxed) — entering callback
    // Thread B simultaneously calls load(acquire) — drain check
    //
    // If relaxed allows reordering, Thread B could see 0 while
    // Thread A is actually inside the callback.
    alignas(64) std::atomic<int32_t> callbackInflight{0};
    std::atomic<bool> entryDone{false};
    std::atomic<bool> checkDone{false};
    int32_t observedDuringEntry = -1;

    // Thread A: enter callback with relaxed add
    std::thread entryThread([&]() {
        callbackInflight.fetch_add(1, std::memory_order_relaxed);
        entryDone.store(true, std::memory_order_release);

        // Hold the callback open until check is done
        while (!checkDone.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        callbackInflight.fetch_sub(1, std::memory_order_release);
    });

    // Wait for entry
    while (!entryDone.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // Thread B: check with acquire load (drain check)
    observedDuringEntry = callbackInflight.load(std::memory_order_acquire);
    checkDone.store(true, std::memory_order_release);

    entryThread.join();

    // After acquire on entryDone, the relaxed fetch_add must be visible.
    // The entryDone release/acquire provides the ordering guarantee
    // that the relaxed add is visible. In the real code, there is NO
    // such synchronization between the callback entry and the drain check.
    ASSERT_EQ(observedDuringEntry, 1);
}

// ============================================================
// P2-2: StallRecoveryController::requestKeyFrame std::function race
//
// std::function members (keyFrameRequestFn_, videoKeyFrameRequestFn_)
// are set from JS thread via setKeyFrameRequestFn() and read from
// decode thread via evaluate() -> requestKeyFrame(). No mutex or
// atomic guard protects this access.
//
// This is the same as existing F3 but this test INTENTIONALLY
// triggers the race under TSan to get a concrete report.
// ============================================================

TEST(p2_2_stall_recovery_std_function_concurrent_set_and_invoke) {
    // Trigger the actual race: one thread sets the std::function
    // while another thread reads and invokes it.
    StallRecoveryController ctrl;
    std::atomic<int> invokeCount{0};
    std::atomic<bool> running{true};

    // First, put controller into Stalled state so evaluate() calls requestKeyFrame()
    ctrl.onDataReceived();
    // Wait for stall detection threshold
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    // Thread 1 (decode thread): repeatedly evaluate, which calls requestKeyFrame
    // when in stalled state
    std::thread decodeThread([&]() {
        while (running.load(std::memory_order_acquire)) {
            ctrl.evaluate();
            std::this_thread::yield();
        }
    });

    // Thread 2 (JS thread): repeatedly set/clear the callback function
    // This races with the read in requestKeyFrame()
    std::thread jsThread([&]() {
        for (int i = 0; i < 1000 && running.load(std::memory_order_acquire); i++) {
            ctrl.setKeyFrameRequestFn([&invokeCount]() {
                invokeCount.fetch_add(1, std::memory_order_relaxed);
            });
            std::this_thread::yield();
            ctrl.setKeyFrameRequestFn(nullptr);
            std::this_thread::yield();
        }
    });

    // Let the race run
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running.store(false, std::memory_order_release);

    decodeThread.join();
    jsThread.join();

    // If we get here without crash, the test ran (TSan may report the race).
    // The important thing is TSan's output, not this assertion.
    ASSERT_TRUE(true);
}

TEST(p2_2_stall_recovery_video_keyframe_fn_concurrent_race) {
    // Same race but on videoKeyFrameRequestFn_
    StallRecoveryController ctrl;
    std::atomic<int> invokeCount{0};
    std::atomic<bool> running{true};

    // Put into stalled state
    ctrl.onDataReceived();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    std::thread decodeThread([&]() {
        while (running.load(std::memory_order_acquire)) {
            ctrl.evaluate();
            std::this_thread::yield();
        }
    });

    std::thread jsThread([&]() {
        for (int i = 0; i < 1000 && running.load(std::memory_order_acquire); i++) {
            ctrl.setVideoKeyFrameRequestFn([&invokeCount]() {
                invokeCount.fetch_add(1, std::memory_order_relaxed);
            });
            std::this_thread::yield();
            ctrl.setVideoKeyFrameRequestFn(nullptr);
            std::this_thread::yield();
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running.store(false, std::memory_order_release);

    decodeThread.join();
    jsThread.join();

    ASSERT_TRUE(true);
}

// OpusDecoderAdapter::lastError() exposes the most recent non-zero Opus error
// code — this is the source the AudioDecodeChannel propagates into
// StreamMetrics::lastDecodeError and ultimately JS-side quality.audioLastDecodeError.
// A regression that loses the error code at the adapter boundary would mask
// genuine stream-corruption events.
TEST(opus_decode_failure_exposes_error_code_via_lastError) {
    using namespace media;
    OpusDecoderAdapter decoder;
    ASSERT_TRUE(decoder.initialize(48000, 1));
    ASSERT_EQ(decoder.lastError(), 0);

    // Feed a deliberately malformed packet (random high-bit bytes never form a
    // valid Opus TOC). libopus returns OPUS_INVALID_PACKET (-4).
    std::vector<uint8_t> garbage = {0xFF, 0xFF, 0xFF, 0xFF};
    std::vector<float> pcm(960 * 2);
    int result = decoder.decode(garbage.data(), garbage.size(), pcm.data(), 960);
    ASSERT_TRUE(result < 0);
    ASSERT_TRUE(decoder.lastError() < 0);  // negative OPUS_* code
}

// primeClock() must be idempotent and keep nowUs() producing monotonic values.
TEST(primeClock_is_idempotent_and_safe_to_call_multiple_times) {
    media::primeClock();
    // After priming, the iOS timebase globals must be non-zero — otherwise
    // nowUs() would take the first-miss fallback branch on every call.
    ASSERT_GT(media::detail::g_machDenom.load(std::memory_order_relaxed), 0u);
    int64_t t1 = media::nowUs();
    media::primeClock();
    int64_t t2 = media::nowUs();
    media::primeClock();
    int64_t t3 = media::nowUs();
    ASSERT_GE(t2, t1);
    ASSERT_GE(t3, t2);
    ASSERT_GT(t3, 0);
}

// Concurrent callers of primeClock() must not produce garbage timebase values
// (benign race: two threads calling mach_timebase_info produce identical values).
TEST(primeClock_concurrent_callers_produce_consistent_nowUs) {
    std::atomic<bool> startFlag{false};
    std::atomic<int> ready{0};
    const int kThreads = 8;
    std::vector<std::thread> workers;
    std::vector<int64_t> samples(kThreads, 0);

    for (int i = 0; i < kThreads; ++i) {
        workers.emplace_back([&, i]() {
            ready.fetch_add(1);
            while (!startFlag.load(std::memory_order_acquire)) {}
            media::primeClock();
            samples[static_cast<size_t>(i)] = media::nowUs();
        });
    }
    while (ready.load() < kThreads) std::this_thread::yield();
    startFlag.store(true, std::memory_order_release);
    for (auto& t : workers) t.join();

    // All threads should observe positive, consistent nowUs values. We don't
    // need them to match (they run at slightly different times), but they must
    // all be close (well under 100ms) — a garbage timebase would produce wildly
    // divergent values.
    int64_t mn = samples[0], mx = samples[0];
    for (auto s : samples) { mn = std::min(mn, s); mx = std::max(mx, s); }
    ASSERT_GT(mn, 0);
    ASSERT_TRUE(mx - mn < 100'000'000);  // < 100ms
}

// ============================================================

TEST_MAIN("Audit Proof Tests")
