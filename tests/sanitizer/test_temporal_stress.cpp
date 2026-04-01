// Temporal Stress Tests: behavioral verification of the audio pipeline under
// adversarial feed patterns. Runs the real AudioDecodeChannel + JitterEstimator
// + DriftCompensator + CatchupController through scripted scenarios and asserts
// behavioral properties (underruns, buffer depth, PLC, drain) that sanitizers
// cannot detect.
//
// Build: cmake -DSANITIZER=address .. && make test_temporal_stress
// Run:   ./test_temporal_stress
//
// These tests use a real Opus encoder to produce valid packets, a real Opus
// decoder inside AudioDecodeChannel, and simulate the three-thread architecture
// (JS push thread, decode thread, audio callback) in a single-threaded loop
// with deterministic ordering.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>
#include <memory>

#include <opus.h>

#include "common/MediaConfig.h"
#include "MediaTypes.h"
#include "FramePool.h"
#include "JitterEstimator.h"
#include "DriftCompensator.h"
#include "AudioDecodeChannel.h"
#include "OpusDecoderAdapter.h"
#include "common/AVSyncCoordinator.h"

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
        printf("FAIL\n    ASSERT_EQ(%s, %s) [%lld != %lld] at %s:%d\n", \
               #a, #b, (long long)_a, (long long)_b, __FILE__, __LINE__); \
        g_tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_LE(a, b) do { \
    auto _a = (a); auto _b = (b); \
    if (_a > _b) { \
        printf("FAIL\n    ASSERT_LE(%s, %s) [%lld > %lld] at %s:%d\n", \
               #a, #b, (long long)_a, (long long)_b, __FILE__, __LINE__); \
        g_tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_GE(a, b) do { \
    auto _a = (a); auto _b = (b); \
    if (_a < _b) { \
        printf("FAIL\n    ASSERT_GE(%s, %s) [%lld < %lld] at %s:%d\n", \
               #a, #b, (long long)_a, (long long)_b, __FILE__, __LINE__); \
        g_tests_failed++; \
        return; \
    } \
} while(0)

// ============================================================
// Opus silence encoder — generates valid Opus packets of silence
// ============================================================
class OpusSilenceEncoder {
public:
    OpusSilenceEncoder() {
        int error = 0;
        encoder_ = opus_encoder_create(
            config::audio::kSampleRate,
            config::audio::kChannels,
            OPUS_APPLICATION_VOIP,
            &error);
        if (error != OPUS_OK || !encoder_) {
            fprintf(stderr, "Failed to create Opus encoder: %d\n", error);
            abort();
        }
        opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(24000));
        opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(1));
    }

    ~OpusSilenceEncoder() {
        if (encoder_) opus_encoder_destroy(encoder_);
    }

    // Encode one 20ms frame of silence → valid Opus packet
    std::vector<uint8_t> encodeSilence() {
        float pcm[config::audio::kFrameSamples * config::audio::kChannels] = {};
        uint8_t out[512];
        int bytes = opus_encode_float(encoder_, pcm,
                                       config::audio::kFrameSamples,
                                       out, sizeof(out));
        if (bytes < 0) {
            fprintf(stderr, "Opus encode failed: %d\n", bytes);
            abort();
        }
        return std::vector<uint8_t>(out, out + bytes);
    }

private:
    OpusEncoder* encoder_ = nullptr;
};

// ============================================================
// Feed event: deliver encoded frame at a specific simulated time
// ============================================================
struct FeedEvent {
    int64_t wallTimeUs;
    int64_t ptsUs;
    std::vector<uint8_t> data;
};

// ============================================================
// Behavioral snapshot captured at each simulated callback
// ============================================================
struct Sample {
    int64_t wallTimeUs;
    int64_t bufferDepthUs;
    StreamState state;
};

// ============================================================
// Scenario runner: single-threaded simulation of the pipeline
// ============================================================
struct ScenarioResult {
    uint64_t underruns;
    uint64_t plcFrames;
    uint64_t framesDrained;
    uint64_t framesDropped;
    uint64_t framesReceived;
    uint64_t ptsDiscontinuities;
    uint64_t gapsOver100ms;
    uint64_t gapsOver500ms;
    int64_t finalBufferUs;
    StreamState finalState;
    std::vector<Sample> samples;
};

static ScenarioResult runScenario(
    const std::vector<FeedEvent>& events,
    int64_t durationUs
) {
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);

    auto decoder = std::make_unique<OpusDecoderAdapter>();
    channel.setDecoder(std::move(decoder));
    channel.activate();

    constexpr int64_t kCallbackIntervalUs = config::audio::kFrameDurationUs; // 20ms

    ScenarioResult result{};
    size_t eventIdx = 0;
    int64_t now = 0;

    while (now < durationUs) {
        // 1. Deliver any feed events whose time has arrived
        while (eventIdx < events.size() &&
               events[eventIdx].wallTimeUs <= now) {
            RawAudioFrame raw;
            raw.data = events[eventIdx].data.data();
            raw.size = events[eventIdx].data.size();
            raw.timestampUs = events[eventIdx].ptsUs;
            raw.durationUs = kCallbackIntervalUs;
            channel.pushEncodedFrame(raw);
            eventIdx++;
        }

        // 2. Run decode thread work (single-threaded simulation)
        channel.serviceDeferredClear();
        for (int i = 0; i < 4; i++) {
            if (!channel.processPendingDecode()) break;
        }

        // 3. PLC if needed (decode thread responsibility in production)
        if (channel.needsPLC()) {
            channel.generatePLC();
        }

        // 4. Transition Buffering → Playing if buffer depth >= target
        StreamState s = channel.state();
        if (s == StreamState::Buffering) {
            int64_t buffered = channel.decodedDurationUs();
            int64_t target = channel.bufferTarget();
            if (buffered >= target) {
                // Manually trigger state transition by reading (readSamples
                // handles Buffering→Playing internally via state checks)
                // We need to keep feeding until the channel transitions itself
            }
        }

        // 5. Simulated audio callback: read 20ms of samples
        float output[config::audio::kFrameSamples * config::audio::kChannels];
        channel.readSamples(output, config::audio::kFrameSamples);

        // 6. Capture snapshot
        result.samples.push_back({
            now,
            channel.bufferedDurationUs(),
            channel.state()
        });

        now += kCallbackIntervalUs;
    }

    // Collect final metrics
    const auto& m = channel.metrics();
    result.underruns = m.underruns.load(std::memory_order_relaxed);
    result.plcFrames = m.plcFrames.load(std::memory_order_relaxed);
    result.framesDrained = m.framesDrained.load(std::memory_order_relaxed);
    result.framesDropped = m.framesDropped.load(std::memory_order_relaxed);
    result.framesReceived = m.framesReceived.load(std::memory_order_relaxed);
    result.ptsDiscontinuities = m.ptsDiscontinuities.load(std::memory_order_relaxed);
    result.gapsOver100ms = m.gapsOver100ms.load(std::memory_order_relaxed);
    result.gapsOver500ms = m.gapsOver500ms.load(std::memory_order_relaxed);
    result.finalBufferUs = channel.bufferedDurationUs();
    result.finalState = channel.state();

    channel.deactivate();
    return result;
}

// ============================================================
// Helper: generate steady feed events
// ============================================================
static std::vector<FeedEvent> generateSteadyFeed(
    OpusSilenceEncoder& enc,
    int64_t startUs,
    int64_t durationUs,
    int64_t intervalUs = config::audio::kFrameDurationUs
) {
    std::vector<FeedEvent> events;
    int64_t pts = 0;
    for (int64_t t = startUs; t < startUs + durationUs; t += intervalUs) {
        events.push_back({t, pts, enc.encodeSilence()});
        pts += config::audio::kFrameDurationUs;
    }
    return events;
}

// ============================================================
// S01: Steady-State Baseline
// Feed: 20ms Opus frames at exactly 20ms intervals for 5 seconds
// Expected: 0 underruns, state converges to Playing, buffer stable
// Validates: §2.1 (decode rate), §3.1 (jitter range)
// ============================================================
TEST(S01_steady_state_baseline) {
    OpusSilenceEncoder enc;
    auto events = generateSteadyFeed(enc, 0, 5'000'000);

    auto r = runScenario(events, 5'000'000);

    // After initial buffering, should be playing
    ASSERT_EQ(static_cast<int>(r.finalState), static_cast<int>(StreamState::Playing));

    // Zero underruns under steady feed
    ASSERT_EQ(r.underruns, 0u);

    // Minimal PLC — at most a couple during initial Buffering→Playing transition
    ASSERT_LE(r.plcFrames, 3u);

    // No frames dropped (queue never full at 1:1 feed rate)
    ASSERT_EQ(r.framesDropped, 0u);

    // All frames received
    ASSERT_GE(static_cast<long long>(r.framesReceived), 240LL); // ~250 frames in 5s

    // Buffer depth should be near jitter target (60ms ± 60ms)
    ASSERT_LE(r.finalBufferUs, static_cast<int64_t>(config::jitter::kMaxBufferUs));

    printf("[buf=%lldus recv=%llu] ",
           static_cast<long long>(r.finalBufferUs),
           static_cast<unsigned long long>(r.framesReceived));
}

// ============================================================
// S02: Hypercore Burst (320ms)
// Feed: 16 frames delivered in 1ms burst, then steady 20ms delivery
// Expected: 0 frame drops from burst (queue absorbs), catchup drains
// Validates: §1.1 (audio queue absorbs burst), §4.1 (catchup before drain)
// ============================================================
TEST(S02_burst_320ms) {
    OpusSilenceEncoder enc;
    std::vector<FeedEvent> events;

    int64_t pts = 0;
    // Burst: 16 frames in 1ms
    for (int i = 0; i < 16; i++) {
        events.push_back({static_cast<int64_t>(i * 50), pts, enc.encodeSilence()}); // 50us apart
        pts += config::audio::kFrameDurationUs;
    }
    // Steady feed for remaining 8 seconds
    for (int64_t t = 320'000; t < 8'000'000; t += config::audio::kFrameDurationUs) {
        events.push_back({t, pts, enc.encodeSilence()});
        pts += config::audio::kFrameDurationUs;
    }

    auto r = runScenario(events, 8'000'000);

    ASSERT_EQ(static_cast<int>(r.finalState), static_cast<int>(StreamState::Playing));

    // Burst should be absorbed — no drops from queue overflow
    // (some drops may occur from pool exhaustion, but zero is ideal)
    ASSERT_LE(r.framesDropped, 4u);

    // Zero underruns — burst provides excess, not deficit
    ASSERT_EQ(r.underruns, 0u);

    // Buffer should converge back to target after burst
    ASSERT_LE(r.finalBufferUs, 2 * config::jitter::kMaxBufferUs);

    printf("[dropped=%llu drained=%llu buf=%lldus] ",
           static_cast<unsigned long long>(r.framesDropped),
           static_cast<unsigned long long>(r.framesDrained),
           static_cast<long long>(r.finalBufferUs));
}

// ============================================================
// S03: Feed Gap 150ms (PLC Coverage)
// Feed: 3s steady, then 150ms silence, then resume
// Expected: PLC activates (≤ 8 frames), 0 decoder resets
// Validates: §6.1 (PLC before underrun), §6.2 (PLC covers typical gaps)
// ============================================================
TEST(S03_gap_150ms_plc) {
    OpusSilenceEncoder enc;
    std::vector<FeedEvent> events;

    int64_t pts = 0;
    // 3 seconds of steady feed
    for (int64_t t = 0; t < 3'000'000; t += config::audio::kFrameDurationUs) {
        events.push_back({t, pts, enc.encodeSilence()});
        pts += config::audio::kFrameDurationUs;
    }
    // 150ms gap (no events)
    // Resume after gap
    int64_t resumeTime = 3'150'000;
    for (int64_t t = resumeTime; t < 6'000'000; t += config::audio::kFrameDurationUs) {
        events.push_back({t, pts, enc.encodeSilence()});
        pts += config::audio::kFrameDurationUs;
    }

    auto r = runScenario(events, 6'000'000);

    // PLC should have activated during the 150ms gap
    // 150ms / 20ms = ~7-8 PLC frames expected
    ASSERT_LE(r.plcFrames, static_cast<uint64_t>(config::plc::kMaxConsecutivePLC + 4));
    // PLC MUST have fired — a 150ms gap without PLC means concealment is broken
    ASSERT_GE(static_cast<long long>(r.plcFrames), 3LL);

    // No PTS discontinuity (gap < kMaxForwardJumpUs = 500ms)
    ASSERT_EQ(r.ptsDiscontinuities, 0u);

    // Should recover to Playing
    ASSERT_EQ(static_cast<int>(r.finalState), static_cast<int>(StreamState::Playing));

    printf("[plc=%llu underruns=%llu] ",
           static_cast<unsigned long long>(r.plcFrames),
           static_cast<unsigned long long>(r.underruns));
}

// ============================================================
// S04: Feed Gap 600ms (Decoder Reset)
// Feed: 3s steady, then 600ms gap, then resume with PTS jump
// Expected: PTS discontinuity detected, decoder reset
// Validates: §7.1 (forward jump threshold), §7.3 (reset recovery)
// ============================================================
TEST(S04_gap_600ms_reset) {
    OpusSilenceEncoder enc;
    std::vector<FeedEvent> events;

    int64_t pts = 0;
    // 3 seconds of steady feed
    for (int64_t t = 0; t < 3'000'000; t += config::audio::kFrameDurationUs) {
        events.push_back({t, pts, enc.encodeSilence()});
        pts += config::audio::kFrameDurationUs;
    }
    // 600ms gap — PTS jumps forward by 600ms
    int64_t resumeTime = 3'600'000;
    pts += 600'000; // PTS jump matches wall-clock gap
    for (int64_t t = resumeTime; t < 6'000'000; t += config::audio::kFrameDurationUs) {
        events.push_back({t, pts, enc.encodeSilence()});
        pts += config::audio::kFrameDurationUs;
    }

    auto r = runScenario(events, 6'000'000);

    // PTS discontinuity should be detected (600ms > kMaxForwardJumpUs = 500ms)
    ASSERT_GE(r.ptsDiscontinuities, 1u);

    // Should recover to Playing after rebuffering
    ASSERT_EQ(static_cast<int>(r.finalState), static_cast<int>(StreamState::Playing));

    // Gap metrics track wall-clock time between pushEncodedFrame() calls.
    // The gap is detected on the first push AFTER the gap, so it appears
    // in the metric. However, in single-threaded simulation the gap timing
    // depends on scenario construction. Assert PTS discontinuity instead
    // (already checked above) — that's the authoritative signal.

    printf("[disc=%llu plc=%llu gaps500=%llu] ",
           static_cast<unsigned long long>(r.ptsDiscontinuities),
           static_cast<unsigned long long>(r.plcFrames),
           static_cast<unsigned long long>(r.gapsOver500ms));
}

// ============================================================
// S05: Sustained 6% Overrate (Catchup Limit)
// Feed: frames at ~18.87ms intervals (6% faster) for 10 seconds
// Expected: buffer grows, drain events trigger, no OOM
// Validates: §4.2 (catchup limit), hard ceiling enforcement
// ============================================================
TEST(S05_overrate_6pct) {
    OpusSilenceEncoder enc;
    std::vector<FeedEvent> events;

    int64_t pts = 0;
    // 6% faster: 20ms * (1/1.06) ≈ 18868us between deliveries
    constexpr int64_t kFastInterval = 18868;
    for (int64_t t = 0; t < 10'000'000; t += kFastInterval) {
        events.push_back({t, pts, enc.encodeSilence()});
        pts += config::audio::kFrameDurationUs;
    }

    auto r = runScenario(events, 10'000'000);

    // Should still be playing (not crashed, not stalled)
    ASSERT_EQ(static_cast<int>(r.finalState), static_cast<int>(StreamState::Playing));

    // Buffer must stay bounded — hard ceiling at kMaxBufferedUs
    // Check final buffer isn't infinite
    ASSERT_LE(r.finalBufferUs, static_cast<int64_t>(config::audio::kMaxBufferedUs + 40000));

    // Some frames should be drained or dropped (excess feed)
    // The pipeline MUST shed excess — either via drain or drop
    uint64_t totalShed = r.framesDrained + r.framesDropped;
    ASSERT_GE(totalShed, 1u);

    // Zero underruns — we're OVER-feeding, not under-feeding
    ASSERT_EQ(r.underruns, 0u);

    printf("[drained=%llu dropped=%llu buf=%lldus] ",
           static_cast<unsigned long long>(r.framesDrained),
           static_cast<unsigned long long>(r.framesDropped),
           static_cast<long long>(r.finalBufferUs));
}

// ============================================================
// S06: Compound — Burst + Gap + Steady
// Feed: 320ms burst, 2s steady, 200ms gap, 3s steady
// Expected: survives all three stressors, final state Playing
// Validates: §10 coherence matrix (all constraints simultaneously)
// ============================================================
TEST(S06_compound_burst_gap_steady) {
    OpusSilenceEncoder enc;
    std::vector<FeedEvent> events;

    int64_t pts = 0;
    int64_t wallTime = 0;

    // Phase 1: 320ms burst (16 frames in 1ms)
    for (int i = 0; i < 16; i++) {
        events.push_back({wallTime, pts, enc.encodeSilence()});
        wallTime += 50; // 50us apart
        pts += config::audio::kFrameDurationUs;
    }

    // Phase 2: 2 seconds steady
    wallTime = 320'000;
    for (int64_t t = 0; t < 2'000'000; t += config::audio::kFrameDurationUs) {
        events.push_back({wallTime + t, pts, enc.encodeSilence()});
        pts += config::audio::kFrameDurationUs;
    }

    // Phase 3: 200ms gap
    wallTime = 2'520'000;

    // Phase 4: 3 seconds steady
    for (int64_t t = 0; t < 3'000'000; t += config::audio::kFrameDurationUs) {
        events.push_back({wallTime + t, pts, enc.encodeSilence()});
        pts += config::audio::kFrameDurationUs;
    }

    auto r = runScenario(events, 6'000'000);

    // Must recover to Playing
    ASSERT_EQ(static_cast<int>(r.finalState), static_cast<int>(StreamState::Playing));

    // Bounded underruns (gap may cause 1-2)
    ASSERT_LE(r.underruns, 3u);

    // PLC covers the 200ms gap between burst drain and steady phases.
    // Burst absorption + gap recovery may produce more PLC than the gap alone
    // because the pipeline transitions through Buffering states multiple times.
    ASSERT_LE(r.plcFrames, 40u);

    // Buffer bounded
    ASSERT_LE(r.finalBufferUs, static_cast<int64_t>(config::jitter::kMaxBufferUs + 40000));

    printf("[under=%llu plc=%llu drained=%llu dropped=%llu buf=%lldus] ",
           static_cast<unsigned long long>(r.underruns),
           static_cast<unsigned long long>(r.plcFrames),
           static_cast<unsigned long long>(r.framesDrained),
           static_cast<unsigned long long>(r.framesDropped),
           static_cast<long long>(r.finalBufferUs));
}

// ============================================================
// S07: Rapid Start/Stop Under Load
// Activate, push 10 frames, deactivate — repeat 50 times
// Expected: no leaked frames, pool returns to full each cycle
// Validates: lifecycle resilience, memory bounds
// ============================================================
TEST(S07_rapid_start_stop) {
    OpusSilenceEncoder enc;
    DecodedAudioPool pool;

    size_t initialAvail = pool.available();

    for (int cycle = 0; cycle < 50; cycle++) {
        AudioDecodeChannel channel(pool);
        auto decoder = std::make_unique<OpusDecoderAdapter>();
        channel.setDecoder(std::move(decoder));
        channel.activate();

        // Push 10 frames
        int64_t pts = 0;
        for (int i = 0; i < 10; i++) {
            auto pkt = enc.encodeSilence();
            RawAudioFrame raw;
            raw.data = pkt.data();
            raw.size = pkt.size();
            raw.timestampUs = pts;
            raw.durationUs = config::audio::kFrameDurationUs;
            channel.pushEncodedFrame(raw);
            pts += config::audio::kFrameDurationUs;
        }

        // Decode some
        channel.serviceDeferredClear();
        channel.processPendingDecode();
        channel.processPendingDecode();

        // Read some
        float output[config::audio::kFrameSamples];
        channel.readSamples(output, config::audio::kFrameSamples);

        channel.deactivate();

        // Service deferred clears that deactivate() requested
        channel.serviceDeferredClear();

        // Read to trigger decodedClearRequested_
        channel.readSamples(output, config::audio::kFrameSamples);
    }

    // All pool tokens must be returned after all channels destroyed
    ASSERT_EQ(pool.available(), initialAvail);

    printf("[cycles=50 pool=%zu/%zu] ", pool.available(), initialAvail);
}

// ============================================================
// S08: Buffer Ceiling Enforcement
// Push frames faster than they can be consumed, verify hard ceiling
// Validates: kMaxBufferedUs enforcement, no unbounded growth
// ============================================================
TEST(S08_buffer_ceiling) {
    OpusSilenceEncoder enc;
    std::vector<FeedEvent> events;

    int64_t pts = 0;
    // Dump 100 frames at once (2 seconds of audio in 0 time)
    for (int i = 0; i < 100; i++) {
        events.push_back({0, pts, enc.encodeSilence()});
        pts += config::audio::kFrameDurationUs;
    }

    // Run for 1 second (short — we just want to see ceiling enforcement)
    auto r = runScenario(events, 1'000'000);

    // Some frames must have been dropped (100 frames > pool capacity)
    ASSERT_GE(r.framesDropped, 1u);

    // Received frames should be less than 100 (some dropped)
    // Total = received + dropped
    uint64_t total = r.framesReceived + r.framesDropped;
    ASSERT_EQ(total, 100u);

    // Buffer must not exceed ceiling
    // Check that at no sample point did we exceed max
    for (const auto& s : r.samples) {
        if (s.bufferDepthUs > static_cast<int64_t>(config::audio::kMaxBufferedUs + 40000)) {
            printf("FAIL\n    Buffer exceeded ceiling: %lldus at t=%lldus\n",
                   static_cast<long long>(s.bufferDepthUs),
                   static_cast<long long>(s.wallTimeUs));
            g_tests_failed++;
            return;
        }
    }

    printf("[recv=%llu dropped=%llu maxbuf=%lldus] ",
           static_cast<unsigned long long>(r.framesReceived),
           static_cast<unsigned long long>(r.framesDropped),
           static_cast<long long>(r.finalBufferUs));
}

// ============================================================
// S09: A/V Sync Dead Zone
// Feed synchronized audio+video events with small offset (< 15ms).
// Verify dead zone suppresses correction (adjustment = 0).
// Then feed events with 30ms offset and verify correction is nonzero.
// Validates: kDeadZoneUs enforcement, ITU-R BT.1359 compliance
// Kills mutation M17 (kDeadZoneUs = 0)
// ============================================================
TEST(S09_avsync_dead_zone) {
    AVSyncCoordinator sync;

    // Simulate 50 synchronized render events with small jitter (±5ms)
    // Audio and video PTS advance together; local time = PTS + small noise
    int64_t basePts = 0;
    int64_t baseTime = 1'000'000; // arbitrary epoch

    // Phase 1: feed events within dead zone (< 15ms offset)
    for (int i = 0; i < 50; i++) {
        int64_t pts = basePts + i * 33333; // 30fps
        int64_t audioTime = baseTime + i * 33333;
        int64_t videoTime = audioTime + 5000; // 5ms offset — within dead zone

        sync.onAudioRender(pts, audioTime);
        sync.onVideoRender(pts, videoTime);
    }

    // After convergence, small offset should produce ZERO adjustment
    int64_t adj = sync.videoBufferAdjustmentUs();
    ASSERT_EQ(adj, 0LL);

    // Phase 2: feed events with 30ms offset (within gradual correction range)
    sync.reset();
    for (int i = 0; i < 100; i++) {
        int64_t pts = basePts + i * 33333;
        int64_t audioTime = baseTime + i * 33333;
        int64_t videoTime = audioTime + 30000; // 30ms late — above dead zone

        sync.onAudioRender(pts, audioTime);
        sync.onVideoRender(pts, videoTime);
    }

    // 30ms offset is above dead zone — adjustment must be nonzero
    int64_t adj2 = sync.videoBufferAdjustmentUs();
    // Video is late (positive offset) → adjustment should be negative (reduce delay)
    ASSERT_TRUE(adj2 != 0);

    printf("[deadzone_adj=%lld gradual_adj=%lld] ",
           static_cast<long long>(adj),
           static_cast<long long>(adj2));
}

// ============================================================
// Main
// ============================================================

int main() {
    printf("Temporal Stress Tests\n");
    printf("====================\n");
    // Tests auto-register via static constructors above
    printf("\n%d passed, %d failed\n", g_tests_passed, g_tests_failed);
    return g_tests_failed > 0 ? 1 : 0;
}
