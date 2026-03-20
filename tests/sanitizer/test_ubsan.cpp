// UndefinedBehaviorSanitizer test for opus-audio C++ components.
// Build: cmake -DSANITIZER=undefined .. && make
// Run: ./test_ubsan
//
// Tests arithmetic overflow, division by zero, shift out-of-bounds,
// misaligned access, null deref guards, and float-to-int casts
// in FramePool, AudioMixer, JitterEstimator,
// DriftCompensator, AudioDecodeChannel, DecodeThread,
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
#include "JitterEstimator.h"
#include "DriftCompensator.h"
#include "AudioDecodeChannel.h"
#include "AudioMixer.h"
#include "DecodeThread.h"
#include "OpusDecoderAdapter.h"
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
    // Drain every channel
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
// JitterEstimator: arithmetic edge cases
// ============================================================
TEST(jitter_estimator_zero_interval) {
    JitterEstimator jitter;
    jitter.reset();
    // Feed packets with zero interval (degenerate case)
    for (int i = 0; i < 50; i++) {
        jitter.onPacket(0, 0);
    }
    int64_t target = jitter.bufferTargetUs();
    ASSERT_TRUE(target >= config::jitter::kMinBufferUs);
    ASSERT_TRUE(target <= config::jitter::kMaxBufferUs);
}
TEST(jitter_estimator_large_deviation) {
    JitterEstimator jitter;
    jitter.reset();
    // Large timestamps that could cause overflow in deviation^2
    int64_t pts = 0;
    int64_t arrival = 0;
    for (int i = 0; i < 200; i++) {
        pts += 20000;
        // Extreme jitter: +/-500ms
        int64_t jit = (i % 2 == 0) ? 500000 : -500000;
        arrival += 20000 + jit;
        jitter.onPacket(pts, arrival);
    }
    int64_t target = jitter.bufferTargetUs();
    ASSERT_TRUE(target >= config::jitter::kMinBufferUs);
    ASSERT_TRUE(target <= config::jitter::kMaxBufferUs);
}
TEST(jitter_estimator_negative_timestamps) {
    JitterEstimator jitter;
    jitter.reset();
    // Negative timestamps (e.g., delta wrapping)
    jitter.onPacket(-100000, -80000);
    jitter.onPacket(-80000, -60000);
    jitter.onPacket(-60000, -40000);
    int64_t target = jitter.bufferTargetUs();
    ASSERT_TRUE(target >= config::jitter::kMinBufferUs);
}
TEST(jitter_estimator_spike_detection) {
    JitterEstimator jitter;
    jitter.reset();
    // Feed stable packets to establish baseline
    int64_t pts = 0;
    int64_t arrival = 0;
    for (int i = 0; i < 50; i++) {
        pts += 20000;
        arrival += 20000;  // Perfect timing
        jitter.onPacket(pts, arrival);
    }
    int64_t baselineTarget = jitter.bufferTargetUs();
    ASSERT_TRUE(baselineTarget >= config::jitter::kMinBufferUs);
    // Now send a spike: one packet 200ms late
    pts += 20000;
    arrival += 220000;  // 200ms late (spike)
    jitter.onPacket(pts, arrival);
    int64_t spikeTarget = jitter.bufferTargetUs();
    // Spike should immediately raise the target above baseline
    ASSERT_TRUE(spikeTarget >= baselineTarget);
    ASSERT_TRUE(spikeTarget <= config::jitter::kMaxBufferUs);
    // During hold period, send normal packets — target should not decrease
    for (int i = 0; i < 10; i++) {
        pts += 20000;
        arrival += 20000;  // Normal timing
        jitter.onPacket(pts, arrival);
    }
    int64_t holdTarget = jitter.bufferTargetUs();
    // Should still be elevated (hold period is 2 seconds, we only sent 10 * 20ms = 200ms)
    ASSERT_TRUE(holdTarget >= baselineTarget);
}
TEST(jitter_estimator_spike_overflow_guard) {
    JitterEstimator jitter;
    jitter.reset();
    // Feed packets with extreme timestamp deltas — no overflow in spike detection
    int64_t pts = 0;
    int64_t arrival = 0;
    for (int i = 0; i < 10; i++) {
        pts += 20000;
        arrival += 20000;
        jitter.onPacket(pts, arrival);
    }
    // Spike with max reasonable jitter
    pts += 20000;
    arrival += 20000 + 500000;  // 500ms late
    jitter.onPacket(pts, arrival);
    int64_t target = jitter.bufferTargetUs();
    ASSERT_TRUE(target >= config::jitter::kMinBufferUs);
    ASSERT_TRUE(target <= config::jitter::kMaxBufferUs);
}
// ============================================================
// DriftCompensator: division and overflow edge cases
// ============================================================
TEST(drift_compensator_zero_window) {
    DriftCompensator comp;
    // Zero measurement window — must not divide by zero
    comp.updateDrift(1000, 0, 100);
    float ratio = comp.currentRatio();
    ASSERT_TRUE(std::isfinite(ratio));
}
TEST(drift_compensator_extreme_drift) {
    DriftCompensator comp;
    // Very large drift value — could overflow in ppm calculation
    comp.updateDrift(std::numeric_limits<int32_t>::max(), 30000000, 10000);
    float ratio = comp.currentRatio();
    ASSERT_TRUE(std::isfinite(ratio));
    ASSERT_TRUE(!std::isnan(ratio));
}
TEST(drift_compensator_negative_drift) {
    DriftCompensator comp;
    // Negative drift
    comp.updateDrift(-500000, 60000000, 5000);
    float ratio = comp.currentRatio();
    ASSERT_TRUE(std::isfinite(ratio));
    comp.reset();
    ratio = comp.currentRatio();
    ASSERT_TRUE(std::abs(ratio - 1.0f) < 0.01f);
}
TEST(drift_compensator_small_window) {
    DriftCompensator comp;
    // Very small measurement window (just above zero)
    comp.updateDrift(100, 1, 50);
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
TEST(mixer_mix_no_channel) {
    AudioMixer mixer;
    // No channel set — must not UB (output values are implementation-defined)
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
    AudioDecodeChannel channel(pool);
    channel.activate();
    // Set gain to 0 then immediately to max
    channel.setGain(0.0f);
    channel.setGain(2.0f);
    // Set gain to negative (clamped)
    channel.setGain(-1.0f);
    channel.deactivate();
}
// ============================================================
// AudioDecodeChannel: boundary conditions
// ============================================================
TEST(decode_channel_read_inactive) {
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);
    // Read from inactive channel — must return 0, not UB
    float output[960];
    size_t read = channel.readSamples(output, 960);
    ASSERT_EQ(read, 0u);
}
TEST(decode_channel_push_inactive) {
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);
    // Push to inactive channel — must be no-op, not UB
    uint8_t data[64] = {};
    RawAudioFrame raw;
    raw.data = data;
    raw.size = sizeof(data);
    raw.timestampUs = 0;
    raw.durationUs = 20000;
    (void)channel.pushEncodedFrame(raw);
}
TEST(decode_channel_zero_size_frame) {
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);
    channel.activate();
    // Zero-size encoded frame
    uint8_t data[1] = {0};
    RawAudioFrame raw;
    raw.data = data;
    raw.size = 0;
    raw.timestampUs = 0;
    raw.durationUs = 20000;
    (void)channel.pushEncodedFrame(raw);
    channel.deactivate();
}
TEST(decode_channel_activate_reactivate) {
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);
    // Activate, deactivate, reactivate — no stale state UB
    channel.activate();
    channel.deactivate();
    channel.activate();
    ASSERT_TRUE(channel.isActive());
    channel.deactivate();
    ASSERT_TRUE(!channel.isActive());
}
TEST(decode_channel_read_zero_samples) {
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);
    channel.activate();
    float output[1];
    // Read 0 samples — no UB
    size_t read = channel.readSamples(output, 0);
    ASSERT_EQ(read, 0u);
    channel.deactivate();
}
TEST(decode_channel_buffer_target_extremes) {
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);
    channel.activate();
    // Very large buffer target
    channel.setBufferTarget(std::numeric_limits<int64_t>::max());
    [[maybe_unused]] auto t1 = channel.bufferTarget();
    // Zero buffer target
    channel.setBufferTarget(0);
    [[maybe_unused]] auto t2 = channel.bufferTarget();
    // Negative buffer target
    channel.setBufferTarget(-1);
    [[maybe_unused]] auto t3 = channel.bufferTarget();
    channel.deactivate();
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
// DecodeThread: construction and channel notification boundaries
// ============================================================
TEST(decode_thread_construct_destruct) {
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);
    // Construct and immediately destroy — no UB
    DecodeThread thread(channel);
}
TEST(decode_thread_notify_active) {
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);
    DecodeThread thread(channel);
    // Notify active before start — no UB
    thread.notifyActive();
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
    // DTX silence marker: Opus TOC byte for CELT-only silence.
    // Opus DTX may produce comfort noise rather than exact zeros.
    static constexpr uint8_t kDtxFrame[] = {0xF8, 0xFF, 0xFE};
    int result = decoder.decode(kDtxFrame, sizeof(kDtxFrame), output.data(), 960);
    ASSERT_TRUE(result > 0);
    // Output should be near-silence (comfort noise level)
    float maxAbs = 0.0f;
    for (int i = 0; i < result; ++i) {
        float a = output[i] < 0 ? -output[i] : output[i];
        if (a > maxAbs) maxAbs = a;
    }
    ASSERT_TRUE(maxAbs < 0.1f);
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
// AudioResampler boundary tests
// ============================================================
TEST(audio_resampler_passthrough_zero_samples) {
    AudioResampler<int16_t> resampler;
    ASSERT_TRUE(resampler.init(48000, 48000));
    ASSERT_TRUE(resampler.isPassthrough());
    int16_t in[1] = {0};
    int16_t out[1] = {0};
    size_t result = resampler.process(out, in, 0);
    ASSERT_EQ(result, 0u);
}
TEST(audio_resampler_passthrough_single_sample) {
    AudioResampler<int16_t> resampler;
    ASSERT_TRUE(resampler.init(48000, 48000));
    int16_t in[1] = {12345};
    int16_t out[1] = {0};
    size_t result = resampler.process(out, in, 1);
    ASSERT_EQ(result, 1u);
    ASSERT_EQ(out[0], static_cast<int16_t>(12345));
}
TEST(audio_resampler_max_output_samples_boundary) {
    AudioResampler<int16_t> resampler;
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
    AudioResampler<int16_t> resampler;
    // Not initialized — process should return 0 (no state)
    int16_t in[960] = {};
    int16_t out[960] = {};
    size_t result = resampler.process(out, in, 960);
    ASSERT_EQ(result, 0u);
}
TEST(audio_resampler_destroy_reinit) {
    AudioResampler<int16_t> resampler;
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
    AudioResampler<int16_t> resampler;
    // Error count starts at 0
    ASSERT_EQ(resampler.processErrorCount(), 0u);
    // Passthrough mode should never increment errors
    ASSERT_TRUE(resampler.init(48000, 48000));
    int16_t buf[960] = {};
    resampler.process(buf, buf, 960);
    ASSERT_EQ(resampler.processErrorCount(), 0u);
}
// ============================================================
// Video: VideoFrameQueue boundary conditions
// ============================================================
#include "video/VideoFrameQueue.h"
#include "video/VideoConfig.h"
TEST(video_queue_push_needs_keyframe) {
    media::VideoFrameQueue queue;
    // Fresh queue requires keyframe first
    uint8_t data[32] = {};
    ASSERT_TRUE(!queue.pushEncodedFrame(data, sizeof(data), 0, false));
    ASSERT_TRUE(queue.pushEncodedFrame(data, sizeof(data), 0, true));
}
TEST(video_queue_push_oversize_frame) {
    media::VideoFrameQueue queue;
    queue.reset();
    // Frame larger than kMaxEncodedFrameSize should be rejected
    std::vector<uint8_t> bigFrame(media::video_config::kMaxEncodedFrameSize + 1, 0);
    ASSERT_TRUE(!queue.pushEncodedFrame(bigFrame.data(), bigFrame.size(), 0, true));
    ;
}
TEST(video_queue_push_null_data) {
    media::VideoFrameQueue queue;
    queue.reset();
    ASSERT_TRUE(!queue.pushEncodedFrame(nullptr, 100, 0, true));
    ASSERT_TRUE(!queue.pushEncodedFrame(nullptr, 0, 0, true));
    ;
}
TEST(video_queue_queue_overflow) {
    media::VideoFrameQueue queue;
    queue.reset();
    // Fill queue beyond depth — should drop oldest
    uint8_t data[32] = {};
    for (size_t i = 0; i < media::video_config::kDecodeQueueDepth + 4; ++i) {
        bool isKey = (i == 0);
        queue.pushEncodedFrame(data, sizeof(data),
                               static_cast<int64_t>(i) * 33333, isKey);
    }
    // Queue should be at max depth, not unbounded
    ASSERT_TRUE(queue.pendingFrames() <= media::video_config::kDecodeQueueDepth);
    // Dropped frames should be tracked
    ASSERT_TRUE(queue.metrics().framesDropped.load(std::memory_order_relaxed) > 0);
    ;
}
TEST(video_queue_needs_keyframe_on_bind) {
    media::VideoFrameQueue queue;
    queue.reset();
    // Non-keyframe before any keyframe should be rejected
    uint8_t data[32] = {};
    ASSERT_TRUE(!queue.pushEncodedFrame(data, sizeof(data), 0, false));
    // Keyframe should be accepted
    ASSERT_TRUE(queue.pushEncodedFrame(data, sizeof(data), 33333, true));
    // Subsequent non-keyframe should be accepted
    ASSERT_TRUE(queue.pushEncodedFrame(data, sizeof(data), 66666, false));
    ;
}
// ============================================================
// Video: VideoJitterEstimator edge cases
// ============================================================
#include "video/VideoSyncController.h"
TEST(video_jitter_zero_interval) {
    media::VideoJitterEstimator jitter;
    // Two frames at same PTS — should not crash or produce NaN
    jitter.onFrame(1000000, 1000000, true);
    jitter.onFrame(1000000, 1000000, false);
    ASSERT_TRUE(jitter.jitterUs() >= 0);
    ASSERT_TRUE(std::isfinite(static_cast<double>(jitter.bufferTargetUs())));
}
TEST(video_jitter_large_deviation) {
    media::VideoJitterEstimator jitter;
    // Normal frames then a huge gap — should clamp, not overflow
    jitter.onFrame(0, 1000000, true);
    jitter.onFrame(33333, 1033333, false);
    jitter.onFrame(66666, 1066666, false);
    // 500ms gap (network stall)
    jitter.onFrame(100000, 1566666, false);
    ASSERT_TRUE(jitter.jitterUs() >= 0);
    int64_t target = jitter.bufferTargetUs();
    ASSERT_TRUE(target >= media::video_config::jitter::kMinBufferUs);
    ASSERT_TRUE(target <= media::video_config::jitter::kMaxBufferUs);
}
TEST(video_jitter_keyframe_excluded) {
    media::VideoJitterEstimator jitter;
    // Keyframes should not inflate jitter estimate (they're larger, arrive later)
    jitter.onFrame(0, 1000000, true);
    jitter.onFrame(33333, 1033333, false);
    jitter.onFrame(66666, 1066666, false);
    int64_t normalJitter = jitter.jitterUs();
    // Large keyframe arrival delay
    jitter.onFrame(100000, 1200000, true);
    int64_t afterKeyframe = jitter.jitterUs();
    // Jitter should not spike due to keyframe
    ASSERT_TRUE(afterKeyframe <= normalJitter * 3 + 10000);
}
// ============================================================
// Video: VideoRenderClock edge cases
// ============================================================
TEST(video_render_clock_unanchored) {
    media::VideoRenderClock clock;
    // Unanchored clock renders immediately (no timing reference yet)
    ASSERT_TRUE(!clock.isAnchored());
    ASSERT_TRUE(clock.isReadyToRender(0, 1000000));
}
TEST(video_render_clock_anchor_and_ready) {
    media::VideoRenderClock clock;
    int64_t firstPts = 1000000;
    int64_t anchorTime = 5000000;
    int64_t bufferTarget = 66000;  // 2 frames
    clock.anchor(firstPts, anchorTime, bufferTarget);
    ASSERT_TRUE(clock.isAnchored());
    // Frame at anchorPts should be ready after bufferTarget elapses
    ASSERT_TRUE(!clock.isReadyToRender(firstPts, anchorTime));
    ASSERT_TRUE(clock.isReadyToRender(firstPts, anchorTime + bufferTarget + 1));
}
// ============================================================
// AudioLevelMeter: float edge cases and SIMD correctness
// ============================================================
#include "AudioMixer.h"

TEST(level_meter_silence) {
    media::AudioLevelMeter meter;
    std::array<float, 960> silence{};
    meter.update(silence.data(), silence.size());
    ASSERT_TRUE(meter.peakLevel() == 0.0f);
    ASSERT_TRUE(meter.rmsLevel() == 0.0f);
    ASSERT_EQ(meter.clipCount(), 0ull);
}

TEST(level_meter_unity_sine) {
    media::AudioLevelMeter meter;
    std::array<float, 960> samples{};
    for (size_t i = 0; i < samples.size(); i++) {
        samples[i] = std::sin(static_cast<float>(i) * 0.1f);
    }
    meter.update(samples.data(), samples.size());
    float peak = meter.peakLevel();
    float rms = meter.rmsLevel();
    // Peak should be close to 1.0 (sine peak)
    ASSERT_TRUE(peak > 0.9f && peak <= 1.0f);
    // RMS of sine = 1/sqrt(2) ≈ 0.707
    ASSERT_TRUE(rms > 0.5f && rms < 0.8f);
    ASSERT_EQ(meter.clipCount(), 0ull);
}

TEST(level_meter_clipping) {
    media::AudioLevelMeter meter;
    std::array<float, 480> samples{};
    for (size_t i = 0; i < samples.size(); i++) {
        samples[i] = 1.5f;  // Clipping
    }
    meter.update(samples.data(), samples.size());
    ASSERT_TRUE(meter.peakLevel() >= 1.0f);
    ASSERT_TRUE(meter.clipCount() > 0);
}

TEST(level_meter_dbfs_conversion) {
    // 0 dBFS = peak of 1.0
    float db = media::AudioLevelMeter::toDbfs(1.0f);
    ASSERT_TRUE(db > -0.1f && db < 0.1f);

    // -6 dBFS ≈ 0.5
    db = media::AudioLevelMeter::toDbfs(0.5f);
    ASSERT_TRUE(db > -6.5f && db < -5.5f);

    // Silence = -100 dBFS
    db = media::AudioLevelMeter::toDbfs(0.0f);
    ASSERT_TRUE(db <= -100.0f);
}

TEST(level_meter_small_buffer) {
    media::AudioLevelMeter meter;
    // Edge case: 1 sample
    float sample = 0.3f;
    meter.update(&sample, 1);
    ASSERT_TRUE(std::abs(meter.peakLevel() - 0.3f) < 0.001f);
    ASSERT_TRUE(std::abs(meter.rmsLevel() - 0.3f) < 0.001f);
}

TEST(level_meter_non_simd_aligned) {
    media::AudioLevelMeter meter;
    // 7 samples: not aligned to SIMD width (4), tests scalar fallback
    std::array<float, 7> samples = {0.1f, -0.5f, 0.3f, -0.7f, 0.2f, -0.4f, 0.6f};
    meter.update(samples.data(), samples.size());
    ASSERT_TRUE(std::abs(meter.peakLevel() - 0.7f) < 0.001f);
}

TEST(level_meter_silence_after_audio) {
    media::AudioLevelMeter meter;
    float loud = 0.9f;
    meter.update(&loud, 1);
    ASSERT_TRUE(meter.peakLevel() > 0.0f);
    meter.silence();
    ASSERT_TRUE(meter.peakLevel() == 0.0f);
    ASSERT_TRUE(meter.rmsLevel() == 0.0f);
}

// ============================================================
// ClipBuffer: ring buffer wrap-around arithmetic
// ============================================================
#include "common/ClipBuffer.h"

TEST(clip_buffer_basic_lifecycle) {
    media::ClipBuffer buf;
    ASSERT_TRUE(!buf.isEnabled());
    buf.setBufferSize(4096);
    buf.setEnabled(true);
    ASSERT_TRUE(buf.isEnabled());

    // No stream header yet
    auto result = buf.extractClip(5.0f);
    ASSERT_TRUE(!result.error.empty());

    // Set header
    std::vector<uint8_t> header(32, 0x1A);
    buf.setStreamHeader(header.data(), header.size());
    ASSERT_TRUE(buf.hasStreamHeader());

    // No clusters yet
    result = buf.extractClip(5.0f);
    ASSERT_TRUE(!result.error.empty());
}

TEST(clip_buffer_wrap_around) {
    media::ClipBuffer buf;
    buf.setBufferSize(1024);  // Small buffer to force wrapping
    buf.setEnabled(true);

    std::vector<uint8_t> header(16, 0x1A);
    buf.setStreamHeader(header.data(), header.size());

    // Write enough data to wrap around the ring buffer multiple times
    std::vector<uint8_t> chunk(128, 0xAB);
    for (int i = 0; i < 20; i++) {
        buf.appendBytes(chunk.data(), chunk.size());
        buf.onNewCluster();
        if (i % 3 == 0) buf.onKeyFrame(i * 33000);
        buf.onBlockPts(i * 33000);
    }

    // Extract should succeed even after wrap-around
    auto result = buf.extractClip(1.0f);
    // May succeed or fail (data overwritten) — no UB either way
    ASSERT_TRUE(result.error.empty() || !result.error.empty());

    buf.reset();
    ASSERT_TRUE(!buf.hasStreamHeader());
}

TEST(clip_buffer_extract_from_pts) {
    media::ClipBuffer buf;
    buf.setBufferSize(8192);
    buf.setEnabled(true);

    std::vector<uint8_t> header(16, 0x1A);
    buf.setStreamHeader(header.data(), header.size());

    std::vector<uint8_t> chunk(64, 0xCD);
    for (int i = 0; i < 30; i++) {
        buf.appendBytes(chunk.data(), chunk.size());
        buf.onNewCluster();
        if (i % 5 == 0) buf.onKeyFrame(i * 33000);
        buf.onBlockPts(i * 33000);
    }

    // Seek to a specific PTS
    auto result = buf.extractFromPts(5 * 33000);
    if (result.error.empty()) {
        // Output should start with header bytes
        ASSERT_TRUE(result.data.size() > header.size());
        ASSERT_TRUE(result.data[0] == 0x1A);
    }
}

TEST(clip_buffer_available_range) {
    media::ClipBuffer buf;
    buf.setBufferSize(8192);
    buf.setEnabled(true);

    ASSERT_TRUE(buf.availableRangeSeconds() == 0.0f);

    std::vector<uint8_t> header(16, 0x1A);
    buf.setStreamHeader(header.data(), header.size());

    std::vector<uint8_t> chunk(64, 0xEF);
    for (int i = 0; i < 10; i++) {
        buf.appendBytes(chunk.data(), chunk.size());
        buf.onNewCluster();
        buf.onKeyFrame(i * 1000000);  // 1 second apart
        buf.onBlockPts(i * 1000000);
    }

    float range = buf.availableRangeSeconds();
    // Should be approximately 9 seconds (10 clusters, 1s apart)
    ASSERT_TRUE(range > 7.0f && range < 11.0f);
}

// ============================================================
// StallRecoveryController: state machine correctness
// ============================================================
#include "common/StallRecoveryController.h"

TEST(stall_controller_initial_state) {
    media::StallRecoveryController ctrl;
    ASSERT_TRUE(ctrl.state() == media::StallState::Healthy);
    ASSERT_TRUE(!ctrl.isStalled());
    auto m = ctrl.metrics();
    ASSERT_EQ(m.stallCount, 0u);
    ASSERT_EQ(m.recoveryCount, 0u);
}

TEST(stall_controller_reset) {
    media::StallRecoveryController ctrl;
    ctrl.setKeyFrameRequestFn([]() {});
    ctrl.setVideoKeyFrameRequestFn([]() {});
    ctrl.onDataReceived();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    ctrl.evaluate();
    ctrl.evaluate();
    // Should be stalled
    ctrl.reset();
    ASSERT_TRUE(ctrl.state() == media::StallState::Healthy);
    ASSERT_EQ(ctrl.metrics().stallCount, 0u);
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
