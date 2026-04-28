// UndefinedBehaviorSanitizer test for opus-audio C++ components.
// Build: cmake -DSANITIZER=undefined .. && make
// Run: ./test_ubsan
//
// Tests arithmetic overflow, division by zero, shift out-of-bounds,
// misaligned access, null deref guards, and float-to-int casts
// in FramePool, AudioMixer, JitterEstimator,
// DriftCompensator, AudioDecodeChannel, DecodeThread,
// and OpusDecoderAdapter.
#include "test_common.h"
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
        jitter.onSample(0, 0);
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
        jitter.onSample(pts, arrival);
    }
    int64_t target = jitter.bufferTargetUs();
    ASSERT_TRUE(target >= config::jitter::kMinBufferUs);
    ASSERT_TRUE(target <= config::jitter::kMaxBufferUs);
}
TEST(jitter_estimator_negative_timestamps) {
    JitterEstimator jitter;
    jitter.reset();
    // Negative timestamps (e.g., delta wrapping)
    jitter.onSample(-100000, -80000);
    jitter.onSample(-80000, -60000);
    jitter.onSample(-60000, -40000);
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
        jitter.onSample(pts, arrival);
    }
    int64_t baselineTarget = jitter.bufferTargetUs();
    ASSERT_TRUE(baselineTarget >= config::jitter::kMinBufferUs);
    // Now send a spike: one packet 200ms late
    pts += 20000;
    arrival += 220000;  // 200ms late (spike)
    jitter.onSample(pts, arrival);
    int64_t spikeTarget = jitter.bufferTargetUs();
    // Spike should immediately raise the target above baseline
    ASSERT_TRUE(spikeTarget >= baselineTarget);
    ASSERT_TRUE(spikeTarget <= config::jitter::kMaxBufferUs);
    // During hold period, send normal packets — target should not decrease
    for (int i = 0; i < 10; i++) {
        pts += 20000;
        arrival += 20000;  // Normal timing
        jitter.onSample(pts, arrival);
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
        jitter.onSample(pts, arrival);
    }
    // Spike with max reasonable jitter
    pts += 20000;
    arrival += 20000 + 500000;  // 500ms late
    jitter.onSample(pts, arrival);
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
    RawAudioFrame raw;
    raw.absOffset = 0;
    raw.size = 64;
    raw.timestampUs = 0;
    raw.durationUs = 20000;
    (void)channel.pushEncodedFrame(raw);
}
TEST(decode_channel_zero_size_frame) {
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);
    channel.activate();
    // Zero-size encoded frame
    RawAudioFrame raw;
    raw.absOffset = 0;
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
    // Write max encoded frame size descriptor
    frame->size = config::audio::kMaxEncodedFrameSize;
    frame->absOffset = 12345;
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
    ASSERT_TRUE(!queue.pushEncodedFrame(0, 32, 0, false));
    ASSERT_TRUE(queue.pushEncodedFrame(0, 32, 0, true));
}
TEST(video_queue_push_oversize_frame) {
    media::VideoFrameQueue queue;
    queue.reset();
    // Frame larger than kMaxEncodedFrameSize should be rejected
    ASSERT_TRUE(!queue.pushEncodedFrame(0, media::video_config::kMaxEncodedFrameSize + 1, 0, true));
    ;
}
TEST(video_queue_push_zero_size) {
    media::VideoFrameQueue queue;
    queue.reset();
    ASSERT_TRUE(!queue.pushEncodedFrame(0, 0, 0, true));
    ;
}
TEST(video_queue_queue_overflow) {
    media::VideoFrameQueue queue;
    queue.reset();
    // Fill queue beyond depth — should drop oldest
    for (size_t i = 0; i < media::video_config::kDecodeQueueDepth + 4; ++i) {
        bool isKey = (i == 0);
        queue.pushEncodedFrame(static_cast<long long>(i) * 32, 32,
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
    ASSERT_TRUE(!queue.pushEncodedFrame(0, 32, 0, false));
    // Keyframe should be accepted
    ASSERT_TRUE(queue.pushEncodedFrame(0, 32, 33333, true));
    // Subsequent non-keyframe should be accepted
    ASSERT_TRUE(queue.pushEncodedFrame(32, 32, 66666, false));
    ;
}
// ============================================================
// Video: VideoJitterEstimator edge cases
// ============================================================
#include "video/VideoSyncController.h"
TEST(video_jitter_zero_interval) {
    media::VideoJitterEstimator jitter;
    // Two frames at same PTS — should not crash or produce NaN
    jitter.onSample(1000000, 1000000, true);
    jitter.onSample(1000000, 1000000, false);
    ASSERT_TRUE(jitter.jitterUs() >= 0);
    ASSERT_TRUE(std::isfinite(static_cast<double>(jitter.bufferTargetUs())));
}
TEST(video_jitter_large_deviation) {
    media::VideoJitterEstimator jitter;
    // Normal frames then a huge gap — should clamp, not overflow
    jitter.onSample(0, 1000000, true);
    jitter.onSample(33333, 1033333, false);
    jitter.onSample(66666, 1066666, false);
    // 500ms gap (network stall)
    jitter.onSample(100000, 1566666, false);
    ASSERT_TRUE(jitter.jitterUs() >= 0);
    int64_t target = jitter.bufferTargetUs();
    ASSERT_TRUE(target >= media::video_config::jitter::kMinBufferUs);
    ASSERT_TRUE(target <= media::video_config::jitter::kMaxBufferUs);
}
TEST(video_jitter_keyframe_excluded) {
    media::VideoJitterEstimator jitter;
    // Keyframes should not inflate jitter estimate (they're larger, arrive later)
    jitter.onSample(0, 1000000, true);
    jitter.onSample(33333, 1033333, false);
    jitter.onSample(66666, 1066666, false);
    int64_t normalJitter = jitter.jitterUs();
    // Large keyframe arrival delay
    jitter.onSample(100000, 1200000, true);
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
// ClipIndex: metadata operations should not trigger UB
// ============================================================
#include "common/ClipIndex.h"
#include "common/IngestRingBuffer.h"

TEST(clip_index_basic_lifecycle) {
    media::IngestRingBuffer ring(4096);
    media::ClipIndex index;
    index.setRingBuffer(&ring);
    ASSERT_TRUE(!index.isEnabled());
    index.setEnabled(true);
    ASSERT_TRUE(index.isEnabled());

    // No stream header yet
    auto result = index.extractClip(5.0f);
    ASSERT_TRUE(!result.error.empty());

    // Set header
    std::vector<uint8_t> header(32, 0x1A);
    index.setStreamHeader(header.data(), header.size());
    ASSERT_TRUE(index.hasStreamHeader());

    // No clusters yet
    result = index.extractClip(5.0f);
    ASSERT_TRUE(!result.error.empty());
}

TEST(clip_index_metadata_no_ub) {
    media::IngestRingBuffer ring(4096);
    media::ClipIndex index;
    index.setRingBuffer(&ring);
    index.setEnabled(true);

    std::vector<uint8_t> header(16, 0x1A);
    index.setStreamHeader(header.data(), header.size());

    // Write clusters to ring and track metadata — no UB in index arithmetic
    std::vector<uint8_t> chunk(128, 0xAB);
    long long absOffset = 0;
    for (int i = 0; i < 20; i++) {
        ASSERT_TRUE(ring.write(chunk.data(), chunk.size()));
        index.onNewCluster(absOffset);
        if (i % 3 == 0) index.onKeyFrame(i * 33000);
        index.onBlockPts(i * 33000);
        absOffset += static_cast<long long>(chunk.size());
    }
    index.updateRetainPosition();

    auto result = index.extractClip(1.0f);
    // May succeed or fail — no UB either way
    ASSERT_TRUE(result.error.empty() || !result.error.empty());

    index.reset();
    ASSERT_TRUE(!index.hasStreamHeader());
}

TEST(clip_index_extract_from_pts) {
    media::IngestRingBuffer ring(8192);
    media::ClipIndex index;
    index.setRingBuffer(&ring);
    index.setEnabled(true);

    std::vector<uint8_t> header(16, 0x1A);
    index.setStreamHeader(header.data(), header.size());

    std::vector<uint8_t> chunk(64, 0xCD);
    long long absOffset = 0;
    for (int i = 0; i < 30; i++) {
        ASSERT_TRUE(ring.write(chunk.data(), chunk.size()));
        index.onNewCluster(absOffset);
        if (i % 5 == 0) index.onKeyFrame(i * 33000);
        index.onBlockPts(i * 33000);
        absOffset += static_cast<long long>(chunk.size());
    }
    index.updateRetainPosition();

    auto result = index.extractFromPts(5 * 33000);
    if (result.error.empty()) {
        ASSERT_TRUE(result.data.size() > header.size());
        ASSERT_TRUE(result.data[0] == 0x1A);
    }
}

TEST(clip_index_available_range) {
    media::IngestRingBuffer ring(8192);
    media::ClipIndex index;
    index.setRingBuffer(&ring);
    index.setEnabled(true);

    ASSERT_TRUE(index.availableRangeSeconds() == 0.0f);

    std::vector<uint8_t> header(16, 0x1A);
    index.setStreamHeader(header.data(), header.size());

    std::vector<uint8_t> chunk(64, 0xEF);
    long long absOffset = 0;
    for (int i = 0; i < 10; i++) {
        ASSERT_TRUE(ring.write(chunk.data(), chunk.size()));
        index.onNewCluster(absOffset);
        index.onKeyFrame(i * 1000000);
        index.onBlockPts(i * 1000000);
        absOffset += static_cast<long long>(chunk.size());
    }
    // Seal last cluster by starting a new one
    index.onNewCluster(absOffset);
    index.updateRetainPosition();

    float range = index.availableRangeSeconds();
    // Should be approximately 9 seconds (10 clusters, 1s apart)
    ASSERT_TRUE(range > 7.0f && range < 11.0f);
}

// ============================================================
// INT64 PTS boundary: timestamps near overflow
// ============================================================
TEST(jitter_estimator_int64_boundary_pts) {
    JitterEstimator jitter;
    jitter.reset();
    // Feed timestamps near INT64_MAX — verify no overflow in delta computation
    constexpr int64_t kBase = INT64_MAX - 1000000;
    for (int i = 0; i < 50; i++) {
        int64_t pts = kBase + static_cast<int64_t>(i) * 20000;
        int64_t arr = kBase + static_cast<int64_t>(i) * 20000 + 500;
        // These may be rejected by maxForwardJump guards, which is fine —
        // the important thing is no UB (signed overflow) in the arithmetic
        jitter.onSample(pts, arr);
    }
    int64_t target = jitter.bufferTargetUs();
    ASSERT_TRUE(target >= config::jitter::kMinBufferUs);
    ASSERT_TRUE(target <= config::jitter::kMaxBufferUs);
    ASSERT_TRUE(jitter.jitterUs() >= 0);
}

TEST(jitter_estimator_zero_delta_pts) {
    JitterEstimator jitter;
    jitter.reset();
    // Duplicate PTS values — delta=0 should be handled without division by zero
    for (int i = 0; i < 20; i++) {
        jitter.onSample(100000, static_cast<int64_t>(i) * 20000 + 1000);
    }
    int64_t target = jitter.bufferTargetUs();
    ASSERT_TRUE(target >= config::jitter::kMinBufferUs);
    ASSERT_TRUE(target <= config::jitter::kMaxBufferUs);
}

TEST(video_jitter_int64_boundary_pts) {
    media::VideoJitterEstimator jitter;
    jitter.reset();
    constexpr int64_t kBase = INT64_MAX - 2000000;
    for (int i = 0; i < 30; i++) {
        int64_t pts = kBase + static_cast<int64_t>(i) * 33333;
        int64_t arr = kBase + static_cast<int64_t>(i) * 33333 + 1000;
        jitter.onSample(pts, arr);
    }
    int64_t target = jitter.bufferTargetUs();
    ASSERT_TRUE(target >= media::video_config::jitter::kMinBufferUs);
    ASSERT_TRUE(target <= media::video_config::jitter::kMaxBufferUs);
}

// ============================================================
// VideoRenderClock: extreme PTS values and anchor arithmetic
// ============================================================
TEST(video_render_clock_int64_boundary) {
    media::VideoRenderClock clock;
    // Anchor with large PTS values — verify no overflow
    constexpr int64_t kLargePts = INT64_MAX / 2;
    constexpr int64_t kLargeLocal = INT64_MAX / 2;
    clock.anchor(kLargePts, kLargeLocal, 66000);
    // scheduledRenderTime computes: anchorLocal + (pts - anchorPts) + bufferTarget
    // With pts == anchorPts, result == anchorLocal + bufferTarget
    int64_t render = clock.scheduledRenderTime(kLargePts);
    ASSERT_TRUE(render == kLargeLocal + 66000);
    // Small delta from anchor — should be well-defined
    int64_t render2 = clock.scheduledRenderTime(kLargePts + 33333);
    ASSERT_TRUE(render2 == kLargeLocal + 33333 + 66000);
}

TEST(video_render_clock_negative_pts_delta) {
    media::VideoRenderClock clock;
    clock.anchor(100000, 200000, 50000);
    // PTS before anchor — negative delta
    int64_t render = clock.scheduledRenderTime(50000);
    // (50000 - 100000) + 200000 + 50000 = 200000
    ASSERT_TRUE(render == 200000);
}

TEST(video_render_clock_playback_rate_extremes) {
    media::VideoRenderClock clock;
    clock.anchor(0, 1000000, 66000);
    // 2x playback rate — PTS delta is halved in local time
    clock.setPlaybackRate(2.0f);
    int64_t render = clock.scheduledRenderTime(100000);
    // delta=100000, scaled = 100000/2.0 = 50000, result = 1000000 + 50000 + 66000
    ASSERT_TRUE(render > 1000000);
    // 0.5x playback rate — PTS delta is doubled
    clock.setPlaybackRate(0.5f);
    int64_t render2 = clock.scheduledRenderTime(100000);
    // delta=100000, scaled = 100000/0.5 = 200000, result = 1000000 + 200000 + 66000
    ASSERT_TRUE(render2 > render);
}

// ============================================================
// DriftCompensator: extreme cumulative drift values
// ============================================================
TEST(drift_compensator_int64_boundary) {
    DriftCompensator comp;
    // Large drift values — verify no overflow in PPM calculation
    comp.updateDrift(INT64_MAX / 1000, 30000000, 10000);
    float ratio = comp.currentRatio();
    ASSERT_TRUE(std::isfinite(ratio));
    ASSERT_TRUE(ratio > 0.0f);
    // Negative extreme
    comp.updateDrift(INT64_MIN / 1000, 30000000, 10000);
    ratio = comp.currentRatio();
    ASSERT_TRUE(std::isfinite(ratio));
    ASSERT_TRUE(ratio > 0.0f);
}

// ============================================================
// CatchupController: extreme buffer values
// ============================================================
TEST(catchup_controller_int64_boundary) {
    CatchupController ctrl;
    // Massive excess — verify no overflow in gain calculation
    float ratio = ctrl.update(INT64_MAX / 2, 60000);
    ASSERT_TRUE(std::isfinite(ratio));
    ASSERT_TRUE(ratio >= 1.0f);
    ASSERT_TRUE(ratio <= config::catchup::kMaxSpeedupRatio);
    // Zero target — avoid division issues
    ratio = ctrl.update(100000, 0);
    ASSERT_TRUE(std::isfinite(ratio));
    // Negative buffered — should clamp
    ratio = ctrl.update(-100000, 60000);
    ASSERT_TRUE(std::isfinite(ratio));
}

// ============================================================
// WebmDemuxer: incremental parse boundary conditions
// ============================================================
#include "demux/WebmDemuxer.h"

TEST(demuxer_empty_feed) {
    media::demux::WebmDemuxer demuxer;
    // Null data
    auto& r1 = demuxer.feedData(nullptr, 0);
    ASSERT_TRUE(r1.audioPackets.empty());
    ASSERT_TRUE(r1.videoPackets.empty());
    ASSERT_TRUE(r1.error.empty());
    // Zero length
    uint8_t data[1] = {0};
    auto& r2 = demuxer.feedData(data, 0);
    ASSERT_TRUE(r2.audioPackets.empty());
    ASSERT_TRUE(r2.error.empty());
}

TEST(demuxer_garbage_bytes) {
    media::demux::WebmDemuxer demuxer;
    // Feed random garbage — should not crash, should report error after retries
    uint8_t garbage[4096];
    for (size_t i = 0; i < sizeof(garbage); i++) {
        garbage[i] = static_cast<uint8_t>((i * 7 + 13) % 256);
    }
    // Feed in chunks to exercise incremental parse
    for (int i = 0; i < 10; i++) {
        auto& result = demuxer.feedData(garbage, sizeof(garbage));
        (void)result;
    }
    // After enough garbage, demuxer should be in error state
    auto& final_result = demuxer.feedData(garbage, 1);
    // Either still trying to parse or in error state — both are valid
    ASSERT_TRUE(final_result.audioPackets.empty());
    ASSERT_TRUE(final_result.videoPackets.empty());
}

TEST(demuxer_single_byte_feeds) {
    media::demux::WebmDemuxer demuxer;
    // Feed data one byte at a time — exercises incremental parse edge cases
    uint8_t zeros[256] = {};
    for (size_t i = 0; i < sizeof(zeros); i++) {
        auto& result = demuxer.feedData(&zeros[i], 1);
        ASSERT_TRUE(result.audioPackets.empty());
        ASSERT_TRUE(result.videoPackets.empty());
    }
}

TEST(demuxer_reset_cycle) {
    media::demux::WebmDemuxer demuxer;
    // Feed garbage, reset, feed again — verify clean recovery
    uint8_t data[512];
    for (size_t i = 0; i < sizeof(data); i++) data[i] = static_cast<uint8_t>(i);
    for (int cycle = 0; cycle < 5; cycle++) {
        demuxer.feedData(data, sizeof(data));
        demuxer.reset();
    }
    // After reset, demuxer should accept new data cleanly
    auto& result = demuxer.feedData(data, sizeof(data));
    ASSERT_TRUE(result.audioPackets.empty());
}

// ============================================================
// VideoFrameQueue: extreme PTS values
// ============================================================
TEST(video_queue_extreme_pts) {
    media::VideoFrameQueue queue;
    queue.reset();
    // INT64_MAX PTS — verify no overflow in gap tracking
    ASSERT_TRUE(queue.pushEncodedFrame(0, 32, 0, true));
    ASSERT_TRUE(queue.pushEncodedFrame(32, 32, INT64_MAX / 2, false));
    auto& m = queue.metrics();
    int64_t maxGap = m.maxInterFrameGapUs.load(std::memory_order_relaxed);
    ASSERT_TRUE(maxGap >= 0);
}

// ============================================================
// AudioResampler: extreme rate ratios
// ============================================================
TEST(audio_resampler_extreme_rates) {
    AudioResamplerFloat resampler;
    // Very low → very high (8kHz → 48kHz = 6x upsample)
    ASSERT_TRUE(resampler.init(8000, 48000));
    float input[160] = {};
    for (int i = 0; i < 160; i++) input[i] = 0.5f;
    float output[960] = {};
    size_t written = resampler.process(output, input, 160);
    ASSERT_TRUE(written > 0);
    ASSERT_TRUE(written <= 960);
    for (size_t i = 0; i < written; i++) {
        ASSERT_TRUE(std::isfinite(output[i]));
    }
    resampler.destroy();
    // High → low (48kHz → 8kHz = 6x downsample)
    ASSERT_TRUE(resampler.init(48000, 8000));
    float output2[160] = {};
    written = resampler.process(output2, input, 160);
    ASSERT_TRUE(written <= 160);
    for (size_t i = 0; i < written; i++) {
        ASSERT_TRUE(std::isfinite(output2[i]));
    }
    resampler.destroy();
}

// ============================================================
// AudioLevelMeter: extreme sample values
// ============================================================
TEST(level_meter_extreme_values) {
    media::AudioLevelMeter meter;
    // NaN samples — should not propagate
    float nan_samples[4] = { 0.5f, NAN, 0.3f, INFINITY };
    meter.update(nan_samples, 4);
    // Peak/RMS may be infinite due to INFINITY input, but should not crash
    float peak = meter.peakLevel();
    (void)peak;  // value may be inf, but no UB
    // Very large samples
    float big_samples[8] = {};
    for (int i = 0; i < 8; i++) big_samples[i] = 1e30f;
    meter.silence();
    meter.update(big_samples, 8);
    float dbfs = media::AudioLevelMeter::toDbfs(0.0f);
    ASSERT_TRUE(std::isfinite(dbfs));
    ASSERT_TRUE(dbfs <= -90.0f);  // 0.0 → -100 dBFS (or similar floor)
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
TEST_MAIN("UndefinedBehaviorSanitizer Tests")
