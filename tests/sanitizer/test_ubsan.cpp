// UndefinedBehaviorSanitizer test for opus-audio C++ components.
// Build: cmake -DSANITIZER=undefined .. && make
// Run: ./test_ubsan
//
// Tests arithmetic overflow, division by zero, shift out-of-bounds,
// misaligned access, null deref guards, and float-to-int casts
// in FramePool, AudioMixer, SyncController,
// DriftCompensator, AudioDecodeSlot, DecodeThread,
// and OpusDecoderAdapter.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cassert>
#include <array>
#include <vector>
#include <limits>
#include <memory>

#include "common/MediaConfig.h"
#include "MediaTypes.h"
#include "FramePool.h"
#include "Participant.h"
#include "SyncController.h"
#include "DriftCompensator.h"
#include "AudioDecodeSlot.h"
#include "AudioMixer.h"
#include "DecodeThread.h"
#include "OpusDecoderAdapter.h"
#include "common/SimdUtils.h"
#include "common/AudioResampler.h"

using namespace media;

static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST(name) \
    static void test_##name(); \
    struct TestReg_##name { \
        TestReg_##name() { \
            printf("  %-60s", #name); \
            fflush(stdout); \
            test_##name(); \
            printf("PASS\n"); \
            g_tests_passed++; \
        } \
    }; \
    static TestReg_##name g_reg_##name; \
    static void test_##name()

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        printf("FAIL\n    ASSERT_TRUE(%s) at %s:%d\n", #expr, __FILE__, __LINE__); \
        g_tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    auto _a = (a); auto _b = (b); \
    if (_a != _b) { \
        printf("FAIL\n    ASSERT_EQ(%s, %s) at %s:%d\n", #a, #b, __FILE__, __LINE__); \
        g_tests_failed++; \
        return; \
    } \
} while(0)

// ============================================================
// FramePool: acquire/release boundary conditions
// ============================================================

TEST(pool_exhaust_and_recover) {
    // Exhaust the pool completely, then recover. No UB on boundary.
    DecodedAudioPool pool;
    std::vector<DecodedAudioPool::Token> tokens;

    // Drain every slot
    while (true) {
        auto tok = pool.acquire();
        if (!tok) break;
        tokens.push_back(std::move(tok));
    }

    ASSERT_EQ(pool.available(), 0u);

    // Acquire on empty pool returns null token (no UB)
    auto empty = pool.acquire();
    ASSERT_TRUE(!empty);

    // Release all
    tokens.clear();
    ASSERT_EQ(pool.available(), config::audio::kDecodePoolSize);
}

TEST(pool_token_double_release) {
    DecodedAudioPool pool;
    auto tok = pool.acquire();
    ASSERT_TRUE(tok);

    tok.release();
    tok.release(); // Must be no-op, not double-free
    ASSERT_EQ(pool.available(), config::audio::kDecodePoolSize);
}

TEST(pool_token_get_after_release) {
    DecodedAudioPool pool;
    auto tok = pool.acquire();
    ASSERT_TRUE(tok);

    tok.release();
    // Accessing get() on released token should return nullptr, not dangle
    ASSERT_TRUE(tok.get() == nullptr);
}

TEST(pool_write_max_sample_count) {
    DecodedAudioPool pool;
    auto tok = pool.acquire();
    ASSERT_TRUE(tok);

    DecodedAudioFrame* frame = tok.get();
    // Write to last valid index — boundary check
    frame->sampleCount = config::audio::kFrameSize;
    for (int i = 0; i < config::audio::kFrameSize; i++) {
        frame->samples[i] = static_cast<float>(i) / static_cast<float>(config::audio::kFrameSize);
    }

    ASSERT_EQ(frame->sampleCount, static_cast<uint32_t>(config::audio::kFrameSize));
    tok.release();
}

// ============================================================
// SyncController / JitterEstimator: arithmetic edge cases
// ============================================================

TEST(jitter_estimator_zero_interval) {
    SyncController sync;
    JitterEstimator* j = sync.getJitter(0);
    ASSERT_TRUE(j != nullptr);

    // Feed packets with zero interval (degenerate case)
    for (int i = 0; i < 50; i++) {
        j->onPacket(0, 0);
    }

    int64_t target = j->bufferTargetUs();
    ASSERT_TRUE(target >= config::jitter::kMinBufferUs);
    ASSERT_TRUE(target <= config::jitter::kMaxBufferUs);

    sync.release(0);
}

TEST(jitter_estimator_large_deviation) {
    SyncController sync;
    JitterEstimator* j = sync.getJitter(0);
    ASSERT_TRUE(j != nullptr);

    // Large timestamps that could cause overflow in deviation^2
    int64_t pts = 0;
    int64_t arrival = 0;
    for (int i = 0; i < 200; i++) {
        pts += 20000;
        // Extreme jitter: ±500ms
        int64_t jitter = (i % 2 == 0) ? 500000 : -500000;
        arrival += 20000 + jitter;
        j->onPacket(pts, arrival);
    }

    int64_t target = j->bufferTargetUs();
    ASSERT_TRUE(target >= config::jitter::kMinBufferUs);
    ASSERT_TRUE(target <= config::jitter::kMaxBufferUs);

    sync.release(0);
}

TEST(jitter_estimator_negative_timestamps) {
    SyncController sync;
    JitterEstimator* j = sync.getJitter(0);
    ASSERT_TRUE(j != nullptr);

    // Negative timestamps (e.g., delta wrapping)
    j->onPacket(-100000, -80000);
    j->onPacket(-80000, -60000);
    j->onPacket(-60000, -40000);

    int64_t target = j->bufferTargetUs();
    ASSERT_TRUE(target >= config::jitter::kMinBufferUs);

    sync.release(0);
}

TEST(jitter_estimator_spike_detection) {
    SyncController sync;
    JitterEstimator* j = sync.getJitter(0);
    ASSERT_TRUE(j != nullptr);

    // Feed stable packets to establish baseline
    int64_t pts = 0;
    int64_t arrival = 0;
    for (int i = 0; i < 50; i++) {
        pts += 20000;
        arrival += 20000;  // Perfect timing
        j->onPacket(pts, arrival);
    }

    int64_t baselineTarget = j->bufferTargetUs();
    ASSERT_TRUE(baselineTarget >= config::jitter::kMinBufferUs);

    // Now send a spike: one packet 200ms late
    pts += 20000;
    arrival += 220000;  // 200ms late (spike)
    j->onPacket(pts, arrival);

    int64_t spikeTarget = j->bufferTargetUs();
    // Spike should immediately raise the target above baseline
    ASSERT_TRUE(spikeTarget >= baselineTarget);
    ASSERT_TRUE(spikeTarget <= config::jitter::kMaxBufferUs);

    // During hold period, send normal packets — target should not decrease
    for (int i = 0; i < 10; i++) {
        pts += 20000;
        arrival += 20000;  // Normal timing
        j->onPacket(pts, arrival);
    }

    int64_t holdTarget = j->bufferTargetUs();
    // Should still be elevated (hold period is 2 seconds, we only sent 10 * 20ms = 200ms)
    ASSERT_TRUE(holdTarget >= baselineTarget);

    sync.release(0);
}

TEST(jitter_estimator_spike_overflow_guard) {
    SyncController sync;
    JitterEstimator* j = sync.getJitter(0);
    ASSERT_TRUE(j != nullptr);

    // Feed packets with extreme timestamp deltas — no overflow in spike detection
    int64_t pts = 0;
    int64_t arrival = 0;
    for (int i = 0; i < 10; i++) {
        pts += 20000;
        arrival += 20000;
        j->onPacket(pts, arrival);
    }

    // Spike with max reasonable jitter
    pts += 20000;
    arrival += 20000 + 500000;  // 500ms late
    j->onPacket(pts, arrival);

    int64_t target = j->bufferTargetUs();
    ASSERT_TRUE(target >= config::jitter::kMinBufferUs);
    ASSERT_TRUE(target <= config::jitter::kMaxBufferUs);

    sync.release(0);
}

TEST(sync_controller_participant_id_boundary) {
    SyncController sync;

    // Participant ID 0 (minimum)
    JitterEstimator* j0 = sync.getJitter(0);
    ASSERT_TRUE(j0 != nullptr);

    // Participant ID 63 (maximum for 64-bit bitmask)
    JitterEstimator* j63 = sync.getJitter(63);
    ASSERT_TRUE(j63 != nullptr);

    sync.release(0);
    sync.release(63);
    sync.resetAll();
}

// ============================================================
// DriftCompensator: division and overflow edge cases
// ============================================================

TEST(drift_compensator_zero_window) {
    DriftCompensator comp;

    // Zero measurement window — must not divide by zero
    comp.updateDrift(1000, 0, 100, 0.5f);

    float ratio = comp.currentRatio();
    ASSERT_TRUE(std::isfinite(ratio));
}

TEST(drift_compensator_extreme_drift) {
    DriftCompensator comp;

    // Very large drift value — could overflow in ppm calculation
    comp.updateDrift(std::numeric_limits<int32_t>::max(), 30000000, 10000, 0.9f);

    float ratio = comp.currentRatio();
    ASSERT_TRUE(std::isfinite(ratio));
    ASSERT_TRUE(!std::isnan(ratio));
}

TEST(drift_compensator_negative_drift) {
    DriftCompensator comp;

    // Negative drift
    comp.updateDrift(-500000, 60000000, 5000, 0.8f);

    float ratio = comp.currentRatio();
    ASSERT_TRUE(std::isfinite(ratio));

    comp.reset();
    ratio = comp.currentRatio();
    ASSERT_TRUE(std::abs(ratio - 1.0f) < 0.01f);
}

TEST(drift_compensator_small_window) {
    DriftCompensator comp;

    // Very small measurement window (just above zero)
    comp.updateDrift(100, 1, 50, 0.5f);

    float ratio = comp.currentRatio();
    ASSERT_TRUE(std::isfinite(ratio));
}

// ============================================================
// SpeexDriftResampler: edge cases
// ============================================================

TEST(speex_drift_resampler_zero_count) {
    SpeexDriftResampler resampler;
    ASSERT_TRUE(resampler.init());

    float input[960] = {};
    float output[960] = {};

    // Zero input count — no UB
    size_t consumed = 0;
    size_t written = resampler.process(input, 0, output, 960, consumed);
    ASSERT_EQ(written, 0u);
    ASSERT_EQ(consumed, 0u);

    resampler.destroy();
}

TEST(speex_drift_resampler_extreme_ratio) {
    SpeexDriftResampler resampler;
    ASSERT_TRUE(resampler.init());

    float input[960];
    float output[960];
    for (int i = 0; i < 960; i++) {
        input[i] = static_cast<float>(i) / 960.0f;
    }

    // Max combined ratio (drift 1.05 * catchup 1.05 = ~1.1025)
    resampler.setRatio(1.11f);
    size_t consumed = 0;
    size_t written = resampler.process(input, 960, output, 960, consumed);
    ASSERT_TRUE(written <= 960u);

    // Min combined ratio (drift 0.95 * catchup 0.97 = ~0.9215)
    resampler.setRatio(0.92f);
    consumed = 0;
    written = resampler.process(input, 960, output, 960, consumed);
    ASSERT_TRUE(written <= 960u);

    // Below min combined — should clamp, not UB
    resampler.setRatio(0.5f);
    consumed = 0;
    written = resampler.process(input, 960, output, 960, consumed);
    ASSERT_TRUE(written <= 960u);

    // Above max combined — should clamp, not UB
    resampler.setRatio(2.0f);
    consumed = 0;
    written = resampler.process(input, 960, output, 960, consumed);
    ASSERT_TRUE(written <= 960u);

    resampler.destroy();
}

TEST(catchup_controller_no_ub) {
    CatchupController catchup;

    // Zero values
    float ratio = catchup.update(0, 0);
    ASSERT_TRUE(std::isfinite(ratio));

    // Large values
    ratio = catchup.update(1000000, 60000);
    ASSERT_TRUE(std::isfinite(ratio));
    ASSERT_TRUE(ratio <= 1.06f);  // Allow smoothing overshoot

    // Negative excess (buffer below target)
    ratio = catchup.update(20000, 60000);
    ASSERT_TRUE(std::isfinite(ratio));
}

TEST(catchup_controller_no_slowdown) {
    CatchupController catchup;

    // Buffer well below target — ratio must stay at 1.0 (no preemptive slowdown).
    // Slowdown removed: PLC and jitter buffer handle underrun prevention.
    float ratio = 1.0f;
    for (int i = 0; i < 100; i++) {
        ratio = catchup.update(0, 60000);
    }
    ASSERT_TRUE(std::isfinite(ratio));
    ASSERT_TRUE(std::abs(ratio - 1.0f) < 0.01f);  // Should stay near unity

    catchup.reset();

    // Empty buffer with very large target — still no slowdown
    for (int i = 0; i < 200; i++) {
        ratio = catchup.update(0, 1000000);
    }
    ASSERT_TRUE(std::isfinite(ratio));
    ASSERT_TRUE(std::abs(ratio - 1.0f) < 0.01f);  // No slowdown, stays at unity
}

TEST(catchup_controller_speedup_to_unity_transition) {
    CatchupController catchup;

    // Start with excess (speedup)
    float ratio = 1.0f;
    for (int i = 0; i < 50; i++) {
        ratio = catchup.update(300000, 60000);  // 300ms buffered, 60ms target
    }
    ASSERT_TRUE(ratio > 1.0f);  // Speeding up

    // Suddenly drain buffer — ratio converges to unity (no slowdown)
    for (int i = 0; i < 200; i++) {
        ratio = catchup.update(0, 60000);  // Empty buffer
    }
    ASSERT_TRUE(std::isfinite(ratio));
    ASSERT_TRUE(std::abs(ratio - 1.0f) < 0.01f);  // Converged to unity, not below
}

// ============================================================
// AudioMixer: mixing math and soft-clip
// ============================================================

TEST(mixer_mix_empty_slots) {
    AudioMixer mixer;
    // No slots set — must not UB (output values are implementation-defined)
    float output[960];
    memset(output, 0, sizeof(output));
    (void)mixer.mix(output, 960);

    // Verify output contains finite values (no NaN/Inf from uninitialized reads)
    for (int i = 0; i < 960; i++) {
        ASSERT_TRUE(std::isfinite(output[i]));
    }
}

TEST(mixer_mix_zero_count) {
    AudioMixer mixer;
    float output[1] = {42.0f};

    // Zero sample count — no UB
    (void)mixer.mix(output, 0);
}

TEST(mixer_gain_ramp_boundary) {
    DecodedAudioPool pool;
    AudioDecodeSlot slot(pool);
    slot.setSlotId(0);
    slot.bind(1);

    // Set gain to 0 then immediately to max
    slot.setGain(0.0f);
    slot.setGain(2.0f);

    // Set gain to negative (clamped)
    slot.setGain(-1.0f);

    slot.unbind();
}

// ============================================================
// AudioDecodeSlot: boundary conditions
// ============================================================

TEST(decode_slot_read_unbound) {
    DecodedAudioPool pool;
    AudioDecodeSlot slot(pool);
    slot.setSlotId(0);

    // Read from unbound slot — must return 0, not UB
    float output[960];
    size_t read = slot.readSamples(output, 960);
    ASSERT_EQ(read, 0u);
}

TEST(decode_slot_push_unbound) {
    DecodedAudioPool pool;
    AudioDecodeSlot slot(pool);
    slot.setSlotId(0);

    // Push to unbound slot — must be no-op, not UB
    uint8_t data[64] = {};
    RawAudioFrame raw;
    raw.data = data;
    raw.size = sizeof(data);
    raw.timestampUs = 0;
    raw.durationUs = 20000;
    (void)slot.pushEncodedFrame(raw);
}

TEST(decode_slot_zero_size_frame) {
    DecodedAudioPool pool;
    AudioDecodeSlot slot(pool);
    slot.setSlotId(0);
    slot.bind(1);

    // Zero-size encoded frame
    uint8_t data[1] = {0};
    RawAudioFrame raw;
    raw.data = data;
    raw.size = 0;
    raw.timestampUs = 0;
    raw.durationUs = 20000;
    (void)slot.pushEncodedFrame(raw);

    slot.unbind();
}

TEST(decode_slot_bind_rebind) {
    DecodedAudioPool pool;
    AudioDecodeSlot slot(pool);
    slot.setSlotId(0);

    // Bind, unbind, rebind — no stale state UB
    slot.bind(1);
    slot.unbind();
    slot.bind(2);

    ASSERT_EQ(slot.boundParticipant(), 2);
    ASSERT_TRUE(slot.isBound());

    slot.unbind();
    ASSERT_TRUE(!slot.isBound());
}

TEST(decode_slot_read_zero_samples) {
    DecodedAudioPool pool;
    AudioDecodeSlot slot(pool);
    slot.setSlotId(0);
    slot.bind(1);

    float output[1];
    // Read 0 samples — no UB
    size_t read = slot.readSamples(output, 0);
    ASSERT_EQ(read, 0u);

    slot.unbind();
}

TEST(decode_slot_buffer_target_extremes) {
    DecodedAudioPool pool;
    AudioDecodeSlot slot(pool);
    slot.setSlotId(0);
    slot.bind(1);

    // Very large buffer target
    slot.setBufferTarget(std::numeric_limits<int64_t>::max());
    [[maybe_unused]] auto t1 = slot.bufferTarget();

    // Zero buffer target
    slot.setBufferTarget(0);
    [[maybe_unused]] auto t2 = slot.bufferTarget();

    // Negative buffer target
    slot.setBufferTarget(-1);
    [[maybe_unused]] auto t3 = slot.bufferTarget();

    slot.unbind();
}

// ============================================================
// Participant: state transitions and boundary values
// ============================================================

TEST(participant_extreme_external_id) {
    ParticipantRegistry registry;

    // INT32_MAX external ID
    Participant* p = registry.findOrCreate(std::numeric_limits<int>::max());
    ASSERT_TRUE(p != nullptr);

    // Zero external ID
    Participant* p0 = registry.findOrCreate(0);
    ASSERT_TRUE(p0 != nullptr);

    // Negative external ID
    Participant* pn = registry.findOrCreate(-1);
    ASSERT_TRUE(pn != nullptr);

    registry.reset();
}

TEST(participant_audio_slot_boundary) {
    ParticipantRegistry registry;
    Participant* p = registry.findOrCreate(1);
    ASSERT_TRUE(p != nullptr);

    // Set slot to max value
    p->setAudioSlot(std::numeric_limits<int>::max());
    ASSERT_EQ(p->audioSlot(), std::numeric_limits<int>::max());
    ASSERT_TRUE(p->hasAudioSlot());

    // Set slot to -1 (unset sentinel)
    p->setAudioSlot(-1);
    ASSERT_TRUE(!p->hasAudioSlot());

    registry.reset();
}

TEST(participant_rapid_state_flips) {
    ParticipantRegistry registry;
    Participant* p = registry.findOrCreate(42);
    ASSERT_TRUE(p != nullptr);

    // Rapid state transitions — no UB from enum values
    for (int i = 0; i < 1000; i++) {
        p->setState(i % 2 == 0 ? ParticipantState::Active : ParticipantState::Connected);
        p->setMuted(i % 3 == 0);
        p->updateActivity();
    }

    registry.reset();
}

// ============================================================
// PendingAudioPool: encoded frame boundary
// ============================================================

TEST(pending_frame_max_size) {
    PendingAudioPool pool;
    auto tok = pool.acquire();
    ASSERT_TRUE(tok);

    PendingAudioFrame* frame = tok.get();
    // Write max encoded frame size
    frame->size = config::audio::kMaxEncodedFrameSize;
    memset(frame->data.data(), 0xFF, config::audio::kMaxEncodedFrameSize);

    // Verify no overwrite
    ASSERT_EQ(frame->size, static_cast<size_t>(config::audio::kMaxEncodedFrameSize));
    tok.release();
}

TEST(pending_frame_zero_size) {
    PendingAudioPool pool;
    auto tok = pool.acquire();
    ASSERT_TRUE(tok);

    PendingAudioFrame* frame = tok.get();
    frame->size = 0;
    frame->ptsUs = 0;

    tok.release();
}

// ============================================================
// DecodedAudioFrame: sample value extremes
// ============================================================

TEST(decoded_frame_extreme_sample_values) {
    DecodedAudioPool pool;
    auto tok = pool.acquire();
    ASSERT_TRUE(tok);

    DecodedAudioFrame* frame = tok.get();
    frame->sampleCount = config::audio::kFrameSize;

    // Fill with extreme float values (not NaN/Inf)
    for (int i = 0; i < config::audio::kFrameSize; i++) {
        if (i % 3 == 0) frame->samples[i] = 1.0f;
        else if (i % 3 == 1) frame->samples[i] = -1.0f;
        else frame->samples[i] = 0.0f;
    }

    // Extreme PTS
    frame->ptsUs = std::numeric_limits<int64_t>::max();

    tok.release();
}

// ============================================================
// FrameQueue: boundary operations
// ============================================================

TEST(frame_queue_pop_empty) {
    DecodedAudioQueue queue;

    // Pop from empty queue — must return null, not UB
    auto tok = queue.pop();
    ASSERT_TRUE(!tok);
}

TEST(frame_queue_fill_and_reject) {
    DecodedAudioPool pool;
    DecodedAudioQueue queue;

    // Fill to capacity
    size_t pushed = 0;
    while (pushed < config::audio::kDecodeQueueDepth) {
        auto tok = pool.acquire();
        if (!tok) break;
        if (!queue.push(std::move(tok))) break;
        pushed++;
    }

    // Full — next push should fail cleanly, not UB
    auto tok = pool.acquire();
    if (tok) {
        ASSERT_TRUE(!queue.push(std::move(tok)));
    }

    queue.clear();
    ASSERT_EQ(pool.available(), config::audio::kDecodePoolSize);
}

TEST(frame_queue_clear_empty) {
    DecodedAudioQueue queue;

    // Clear empty queue — no UB
    queue.clear();
    queue.clearWithSync();
    ASSERT_TRUE(queue.empty());
}

// ============================================================
// Stress: repeated operations at boundaries
// ============================================================

TEST(stress_pool_boundary_cycles) {
    DecodedAudioPool pool;

    // Repeatedly exhaust and refill
    for (int cycle = 0; cycle < 100; cycle++) {
        std::vector<DecodedAudioPool::Token> tokens;

        while (true) {
            auto tok = pool.acquire();
            if (!tok) break;
            tok->sampleCount = static_cast<uint32_t>(cycle);
            tok->ptsUs = static_cast<int64_t>(cycle) * 20000;
            tokens.push_back(std::move(tok));
        }

        ASSERT_EQ(pool.available(), 0u);
        tokens.clear();
        ASSERT_EQ(pool.available(), config::audio::kDecodePoolSize);
    }
}

// ============================================================
// DecodeThread: construction and slot notification boundaries
// ============================================================

TEST(decode_thread_construct_destruct) {
    DecodedAudioPool pool;
    std::array<AudioDecodeSlot*, DecodeThread::kMaxAudioSlots> slots{};
    std::array<AudioDecodeSlot, 4> slotStorage = {
        AudioDecodeSlot(pool), AudioDecodeSlot(pool),
        AudioDecodeSlot(pool), AudioDecodeSlot(pool)
    };
    for (int i = 0; i < 4; i++) {
        slotStorage[i].setSlotId(i);
        slots[i] = &slotStorage[i];
    }

    // Construct and immediately destroy — no UB
    DecodeThread thread(slots);
}

TEST(decode_thread_notify_invalid_slot) {
    DecodedAudioPool pool;
    std::array<AudioDecodeSlot*, DecodeThread::kMaxAudioSlots> slots{};
    DecodeThread thread(slots);

    // Out-of-range slot indices — must not shift UB or array OOB
    thread.notifySlotBound(-1);
    thread.notifySlotBound(DecodeThread::kMaxAudioSlots);
    thread.notifySlotBound(999);
    thread.notifySlotUnbound(-1);
    thread.notifySlotUnbound(DecodeThread::kMaxAudioSlots);
}

TEST(decode_thread_notify_boundary_slots) {
    DecodedAudioPool pool;
    std::array<AudioDecodeSlot*, DecodeThread::kMaxAudioSlots> slots{};
    DecodeThread thread(slots);

    // Valid boundary indices
    thread.notifySlotBound(0);
    thread.notifySlotBound(DecodeThread::kMaxAudioSlots - 1);
    thread.notifySlotUnbound(0);
    thread.notifySlotUnbound(DecodeThread::kMaxAudioSlots - 1);
}

TEST(decode_thread_setInactivityCallback_guard) {
    DecodedAudioPool pool;
    std::array<AudioDecodeSlot*, DecodeThread::kMaxAudioSlots> slots{};
    DecodeThread thread(slots);
    ParticipantRegistry registry;

    // Should succeed when thread is not running
    thread.setInactivityCallback(registry, nullptr, nullptr);
}

// ============================================================
// OpusDecoderAdapter: boundary inputs
// ============================================================

TEST(opus_decoder_decode_null_guards) {
    OpusDecoderAdapter decoder;
    ASSERT_TRUE(decoder.initialize(48000, 1));

    std::array<uint8_t, 64> input{};
    std::array<float, 960> output{};

    // Null input
    int r1 = decoder.decode(nullptr, 64, output.data(), 960);
    ASSERT_TRUE(r1 < 0);

    // Null output
    int r2 = decoder.decode(input.data(), 64, nullptr, 960);
    ASSERT_TRUE(r2 < 0);

    // Null PLC output
    int r3 = decoder.decodePLC(nullptr, 960);
    ASSERT_TRUE(r3 < 0);

    // Null FEC output
    int r4 = decoder.decodeFEC(input.data(), 64, nullptr, 960);
    ASSERT_TRUE(r4 < 0);
}

TEST(opus_decoder_uninitialized_decode) {
    OpusDecoderAdapter decoder;
    ASSERT_TRUE(!decoder.isValid());

    std::array<uint8_t, 64> input{};
    std::array<float, 960> output{};

    // Decode on uninitialized decoder — must return error, not UB
    int r1 = decoder.decode(input.data(), 64, output.data(), 960);
    ASSERT_TRUE(r1 < 0);

    int r2 = decoder.decodePLC(output.data(), 960);
    ASSERT_TRUE(r2 < 0);
}

TEST(opus_decoder_dtx_silence_marker) {
    OpusDecoderAdapter decoder;
    ASSERT_TRUE(decoder.initialize(48000, 1));

    std::array<float, 960> output{};

    // Feed the DTX silence marker
    int result = decoder.decode(
        OpusDecoderAdapter::kDtxFrame, sizeof(OpusDecoderAdapter::kDtxFrame),
        output.data(), 960);
    ASSERT_TRUE(result > 0);

    // Output should be silence (zeros)
    bool allZero = true;
    for (int i = 0; i < result; ++i) {
        if (output[i] != 0.0f) { allZero = false; break; }
    }
    ASSERT_TRUE(allZero);
}

TEST(opus_decoder_decode_garbage_input) {
    OpusDecoderAdapter decoder;
    ASSERT_TRUE(decoder.initialize(48000, 1));

    std::array<float, 960> output{};

    // Random garbage bytes — should not crash or UB, just return error
    std::array<uint8_t, 128> garbage{};
    for (size_t i = 0; i < garbage.size(); ++i) {
        garbage[i] = static_cast<uint8_t>((i * 37 + 13) & 0xFF);
    }

    int result = decoder.decode(garbage.data(), garbage.size(), output.data(), 960);
    // May succeed (Opus is lenient) or fail — either is fine, no UB
    (void)result;
}

TEST(opus_decoder_fec_decode) {
    OpusDecoderAdapter decoder;
    ASSERT_TRUE(decoder.initialize(48000, 1));

    // First encode a real frame to seed the decoder state — use PLC to prime instead
    std::array<float, 960> output{};

    // Prime decoder with PLC
    int r0 = decoder.decodePLC(output.data(), 960);
    ASSERT_TRUE(r0 > 0);

    // Random data for FEC decode — should not UB
    std::array<uint8_t, 64> fakeData{};
    for (size_t i = 0; i < fakeData.size(); ++i) {
        fakeData[i] = static_cast<uint8_t>((i * 13 + 7) & 0xFF);
    }

    int r2 = decoder.decodeFEC(fakeData.data(), fakeData.size(), output.data(), 960);
    // May succeed or fail depending on packet content, no UB either way
    (void)r2;
}

TEST(opus_decoder_zero_size_input) {
    OpusDecoderAdapter decoder;
    ASSERT_TRUE(decoder.initialize(48000, 1));

    std::array<float, 960> output{};
    uint8_t dummy[1] = {0};

    // Zero-size input — Opus treats this as PLC internally
    int result = decoder.decode(dummy, 0, output.data(), 960);
    // Should not crash — Opus returns error for 0-length with non-null ptr
    (void)result;
}

// ============================================================
// SimdUtils boundary tests
// ============================================================

TEST(simd_copy_int16_zero_count) {
    int16_t src[4] = {1, 2, 3, 4};
    int16_t dst[4] = {0};
    // Zero count should be a no-op, no UB from memcpy(dst, src, 0)
    simd::copyInt16(dst, src, 0);
    ASSERT_EQ(dst[0], static_cast<int16_t>(0));
}

TEST(simd_copy_int16_single_sample) {
    int16_t src[1] = {32767};
    int16_t dst[1] = {0};
    simd::copyInt16(dst, src, 1);
    ASSERT_EQ(dst[0], static_cast<int16_t>(32767));
}

TEST(simd_copy_int16_max_frame) {
    // Full 960-sample frame (typical Opus frame)
    std::array<int16_t, 960> src;
    std::array<int16_t, 960> dst{};
    for (int i = 0; i < 960; ++i) {
        src[i] = static_cast<int16_t>(i - 480);  // Range -480..479
    }
    simd::copyInt16(dst.data(), src.data(), 960);
    ASSERT_EQ(dst[0], static_cast<int16_t>(-480));
    ASSERT_EQ(dst[959], static_cast<int16_t>(479));
}

TEST(simd_copy_bytes_zero_count) {
    uint8_t src[4] = {0xFF, 0xFE, 0xFD, 0xFC};
    uint8_t dst[4] = {0};
    simd::copyBytes(dst, src, 0);
    ASSERT_EQ(dst[0], static_cast<uint8_t>(0));
}

TEST(simd_zero_int16_boundary) {
    std::array<int16_t, 4> buf = {1, 2, 3, 4};
    // Zero count
    simd::zeroInt16(buf.data(), 0);
    ASSERT_EQ(buf[0], static_cast<int16_t>(1));  // Unchanged

    // Single sample
    simd::zeroInt16(buf.data(), 1);
    ASSERT_EQ(buf[0], static_cast<int16_t>(0));
    ASSERT_EQ(buf[1], static_cast<int16_t>(2));  // Unchanged

    // Full frame
    std::array<int16_t, 960> frame;
    for (auto& s : frame) s = 12345;
    simd::zeroInt16(frame.data(), 960);
    for (int i = 0; i < 960; ++i) {
        ASSERT_EQ(frame[i], static_cast<int16_t>(0));
    }
}

// ============================================================
// AudioResampler boundary tests
// ============================================================

TEST(audio_resampler_passthrough_zero_samples) {
    AudioResamplerInt16 resampler;
    ASSERT_TRUE(resampler.init(48000, 48000));
    ASSERT_TRUE(resampler.isPassthrough());

    int16_t in[1] = {0};
    int16_t out[1] = {0};
    size_t result = resampler.process(out, in, 0);
    ASSERT_EQ(result, 0u);
}

TEST(audio_resampler_passthrough_single_sample) {
    AudioResamplerInt16 resampler;
    ASSERT_TRUE(resampler.init(48000, 48000));

    int16_t in[1] = {12345};
    int16_t out[1] = {0};
    size_t result = resampler.process(out, in, 1);
    ASSERT_EQ(result, 1u);
    ASSERT_EQ(out[0], static_cast<int16_t>(12345));
}

TEST(audio_resampler_max_output_samples_boundary) {
    AudioResamplerInt16 resampler;

    // Passthrough: max output == input
    ASSERT_TRUE(resampler.init(48000, 48000));
    ASSERT_EQ(resampler.maxOutputSamples(960), 960u);
    ASSERT_EQ(resampler.maxOutputSamples(0), 0u);

    // Upsample 16kHz → 48kHz: output = ceil(input * 3)
    ASSERT_TRUE(resampler.init(16000, 48000));
    ASSERT_EQ(resampler.maxOutputSamples(320), 960u);
    ASSERT_EQ(resampler.maxOutputSamples(0), 0u);
    ASSERT_EQ(resampler.maxOutputSamples(1), 3u);
}

TEST(audio_resampler_uninitialized_process) {
    AudioResamplerInt16 resampler;
    // Not initialized — process should return 0 (no state)
    int16_t in[960] = {};
    int16_t out[960] = {};
    size_t result = resampler.process(out, in, 960);
    ASSERT_EQ(result, 0u);
}

TEST(audio_resampler_destroy_reinit) {
    AudioResamplerInt16 resampler;
    ASSERT_TRUE(resampler.init(16000, 48000));
    ASSERT_TRUE(resampler.isInitialized());

    resampler.destroy();
    ASSERT_TRUE(!resampler.isInitialized());

    // Re-init with different rates
    ASSERT_TRUE(resampler.init(44100, 48000));
    ASSERT_TRUE(resampler.isInitialized());
    ASSERT_TRUE(!resampler.isPassthrough());
}

TEST(audio_resampler_float_passthrough) {
    AudioResamplerFloat resampler;
    ASSERT_TRUE(resampler.init(48000, 48000));
    ASSERT_TRUE(resampler.isPassthrough());

    float in[4] = {-1.0f, 0.0f, 0.5f, 1.0f};
    float out[4] = {0};
    size_t result = resampler.process(out, in, 4);
    ASSERT_EQ(result, 4u);
    ASSERT_TRUE(out[0] == -1.0f);
    ASSERT_TRUE(out[3] == 1.0f);
}

TEST(audio_resampler_process_error_count) {
    AudioResamplerInt16 resampler;
    // Error count starts at 0
    ASSERT_EQ(resampler.processErrorCount(), 0u);

    // Passthrough mode should never increment errors
    ASSERT_TRUE(resampler.init(48000, 48000));
    int16_t buf[960] = {};
    resampler.process(buf, buf, 960);
    ASSERT_EQ(resampler.processErrorCount(), 0u);
}

// ============================================================
// Main
// ============================================================

int main() {
    printf("\n=== UndefinedBehaviorSanitizer Tests ===\n\n");
    // Tests are auto-registered via static constructors above
    printf("\n--- Results: %d passed, %d failed ---\n\n", g_tests_passed, g_tests_failed);
    return g_tests_failed > 0 ? 1 : 0;
}
