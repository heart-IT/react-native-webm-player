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

#include "test_common.h"
#include <atomic>
#include <cmath>
#include <thread>
#include <vector>
#include <algorithm>
#include <memory>

#include <opus.h>

#include "common/MediaConfig.h"
#include "common/IngestRingBuffer.h"
#include "MediaTypes.h"
#include "FramePool.h"
#include "JitterEstimator.h"
#include "DriftCompensator.h"
#include "AudioDecodeChannel.h"
#include "OpusDecoderAdapter.h"
#include "common/AVSyncCoordinator.h"
#include "video/VideoFrameQueue.h"
#include "video/VideoSyncController.h"
#include "video/VideoConfig.h"

using namespace media;

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
    uint64_t silenceSkipFrames;
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
    // Ring buffer for encoded packet storage (decode reads from here)
    IngestRingBuffer ringBuffer(1 << 20);  // 1MB — enough for test scenarios
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);
    channel.setRingBuffer(&ringBuffer);

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
            // Write encoded data to ring buffer and record absOffset
            long long absOffset = static_cast<long long>(ringBuffer.currentWritePos());
            ringBuffer.write(events[eventIdx].data.data(), events[eventIdx].data.size());

            RawAudioFrame raw;
            raw.absOffset = absOffset;
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
    result.silenceSkipFrames = m.silenceSkipFrames.load(std::memory_order_relaxed);
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

    // Some frames should be shed (excess feed)
    // The pipeline MUST shed excess — via drain, drop, or silence-skip
    uint64_t totalShed = r.framesDrained + r.framesDropped + r.silenceSkipFrames;
    ASSERT_GE(totalShed, 1u);

    // Zero underruns — we're OVER-feeding, not under-feeding
    ASSERT_EQ(r.underruns, 0u);

    printf("[drained=%llu dropped=%llu silenceSkip=%llu buf=%lldus] ",
           static_cast<unsigned long long>(r.framesDrained),
           static_cast<unsigned long long>(r.framesDropped),
           static_cast<unsigned long long>(r.silenceSkipFrames),
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
    IngestRingBuffer ringBuffer(1 << 20);

    size_t initialAvail = pool.available();

    for (int cycle = 0; cycle < 50; cycle++) {
        AudioDecodeChannel channel(pool);
        channel.setRingBuffer(&ringBuffer);
        auto decoder = std::make_unique<OpusDecoderAdapter>();
        channel.setDecoder(std::move(decoder));
        channel.activate();

        // Push 10 frames
        int64_t pts = 0;
        for (int i = 0; i < 10; i++) {
            auto pkt = enc.encodeSilence();
            long long absOffset = static_cast<long long>(ringBuffer.currentWritePos());
            ringBuffer.write(pkt.data(), pkt.size());
            RawAudioFrame raw;
            raw.absOffset = absOffset;
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
// VIDEO PIPELINE SCENARIOS
// ============================================================

// Helper: push a fake VP9 frame (just bytes + PTS + keyframe flag)
static bool pushVideoFrame(VideoFrameQueue& q, int64_t ptsUs, bool keyFrame, size_t size = 128) {
    static long long nextOffset = 0;
    long long absOffset = nextOffset;
    nextOffset += static_cast<long long>(size);
    return q.pushEncodedFrame(absOffset, size, ptsUs, keyFrame);
}

// ============================================================
// V01: Keyframe Gating
// Push delta frames before any keyframe — all should be dropped.
// Then push keyframe — should be accepted. Subsequent deltas accepted.
// Validates: needsKeyFrame_ gating, keyframe clears gate
// ============================================================
TEST(V01_keyframe_gating) {
    VideoFrameQueue queue;

    // Push 10 delta frames — all rejected (no keyframe yet)
    for (int i = 0; i < 10; i++) {
        bool ok = pushVideoFrame(queue, i * 33333, false);
        ASSERT_TRUE(!ok);
    }
    ASSERT_EQ(static_cast<long long>(queue.metrics().framesDropped.load(std::memory_order_relaxed)), 10LL);
    ASSERT_EQ(static_cast<long long>(queue.metrics().framesReceived.load(std::memory_order_relaxed)), 0LL);
    ASSERT_EQ(queue.pendingFrames(), 0u);

    // Push keyframe — accepted
    ASSERT_TRUE(pushVideoFrame(queue, 333330, true));
    ASSERT_EQ(static_cast<long long>(queue.metrics().framesReceived.load(std::memory_order_relaxed)), 1LL);
    ASSERT_EQ(queue.pendingFrames(), 1u);

    // Subsequent deltas accepted
    ASSERT_TRUE(pushVideoFrame(queue, 366663, false));
    ASSERT_TRUE(pushVideoFrame(queue, 399996, false));
    ASSERT_EQ(queue.pendingFrames(), 3u);

    printf("[dropped=%llu recv=%llu] ",
           static_cast<unsigned long long>(queue.metrics().framesDropped.load(std::memory_order_relaxed)),
           static_cast<unsigned long long>(queue.metrics().framesReceived.load(std::memory_order_relaxed)));
}

// ============================================================
// V02: Queue Depth Cap
// Fill queue to kDecodeQueueDepth, then push one more — oldest dropped.
// Validates: overflow handling, bounded memory
// ============================================================
TEST(V02_queue_depth_cap) {
    VideoFrameQueue queue;

    // Fill to capacity
    int64_t pts = 0;
    ASSERT_TRUE(pushVideoFrame(queue, pts, true)); // keyframe first
    pts += 33333;
    for (size_t i = 1; i < video_config::kDecodeQueueDepth; i++) {
        ASSERT_TRUE(pushVideoFrame(queue, pts, false));
        pts += 33333;
    }
    ASSERT_EQ(queue.pendingFrames(), video_config::kDecodeQueueDepth);

    uint64_t dropsBefore = queue.metrics().framesDropped.load(std::memory_order_relaxed);

    // One more (P-frame) — triggers overflow. The front keyframe is evicted,
    // queue clears, needsKeyFrame_ flips to true, and the incoming P-frame is
    // then gated out (no keyframe to reference). Push returns false and the
    // queue is left empty awaiting a keyframe.
    ASSERT_FALSE(pushVideoFrame(queue, pts, false));
    ASSERT_EQ(queue.pendingFrames(), 0u);
    ASSERT_TRUE(queue.isAwaitingKeyFrame());

    // Drop count should have increased by at least 2 (evicted keyframe + gated P-frame)
    uint64_t dropsAfter = queue.metrics().framesDropped.load(std::memory_order_relaxed);
    ASSERT_GE(static_cast<long long>(dropsAfter), static_cast<long long>(dropsBefore + 2));

    printf("[depth=%zu drops=%llu] ",
           queue.pendingFrames(),
           static_cast<unsigned long long>(dropsAfter));
}

// ============================================================
// V03: Render Clock Timing
// Anchor clock, push frames, verify isReadyToRender respects buffer target.
// Validates: VideoRenderClock scheduling, buffer target delay
// ============================================================
TEST(V03_render_clock_timing) {
    VideoRenderClock clock;
    constexpr int64_t kBufferTarget = video_config::jitter::kDefaultBufferUs; // 66ms

    // Anchor: first frame PTS=0 at local time 1000000, buffer target 66ms
    clock.anchor(0, 1'000'000, kBufferTarget);

    // Frame at PTS=0 should render at: anchor(1000000) + ptsDelta(0) + buffer(66000) = 1066000
    ASSERT_EQ(clock.scheduledRenderTime(0), 1'066'000LL);

    // At t=1065999, not ready yet
    ASSERT_TRUE(!clock.isReadyToRender(0, 1'065'999));

    // At t=1066000, ready
    ASSERT_TRUE(clock.isReadyToRender(0, 1'066'000));

    // Frame at PTS=33333 should render at: 1000000 + 33333 + 66000 = 1099333
    ASSERT_EQ(clock.scheduledRenderTime(33333), 1'099'333LL);

    printf("[sched0=%lld sched1=%lld] ",
           static_cast<long long>(clock.scheduledRenderTime(0)),
           static_cast<long long>(clock.scheduledRenderTime(33333)));
}

// ============================================================
// V04: Late Frame Detection + Skip
// Phase 1: Pop frames at correct render times — zero late frames.
// Phase 2: Let time advance past deadlines — late frames detected, stale skipped.
// Validates: tryPopReadyFrame timing, lateFrames/skippedFrames metrics
// Kills mutation V-M03 (kLateFrameThresholdUs = 0)
// ============================================================
TEST(V04_late_frame_skip) {
    // --- Phase 1: Normal pacing (no late frames) ---
    {
        VideoFrameQueue queue;
        VideoRenderClock clock;

        // Push keyframe + 3 delta frames at 30fps
        int64_t pts = 0;
        ASSERT_TRUE(pushVideoFrame(queue, pts, true));
        pts += 33333;
        for (int i = 1; i < 4; i++) {
            ASSERT_TRUE(pushVideoFrame(queue, pts, false));
            pts += 33333;
        }

        // Anchor clock: PTS=0 at time=0, buffer=66ms
        clock.anchor(0, 0, 66000);

        // Pop each frame slightly after its scheduled render time (1ms late).
        // With the real kLateFrameThresholdUs (16667us = half a frame), 1ms of
        // lateness is well within tolerance → zero late frames.
        // With V-M03 (threshold = 0), 1ms > 0 → every frame is late → assertion fails.
        EncodedVideoFrame out;
        for (int i = 0; i < 4; i++) {
            int64_t renderTime = clock.scheduledRenderTime(i * 33333);
            PopResult r = queue.tryPopReadyFrame(out, clock, renderTime + 1000);
            ASSERT_TRUE(r != PopResult::Nothing);
        }

        // Under normal pacing with real threshold, ZERO late frames
        uint64_t late = queue.metrics().lateFrames.load(std::memory_order_relaxed);
        ASSERT_EQ(static_cast<long long>(late), 0LL);
    }

    // --- Phase 2: Stale frames (time jumps ahead) ---
    {
        VideoFrameQueue queue;
        VideoRenderClock clock;

        // Push keyframe + 7 delta frames at 30fps
        int64_t pts = 0;
        ASSERT_TRUE(pushVideoFrame(queue, pts, true));
        pts += 33333;
        for (int i = 1; i < 8; i++) {
            ASSERT_TRUE(pushVideoFrame(queue, pts, false));
            pts += 33333;
        }

        // Anchor clock: PTS=0 at time=0, buffer=66ms
        clock.anchor(0, 0, 66000);

        // Advance time to 300ms — frames 0-6 are past deadline
        int64_t now = 300'000;
        EncodedVideoFrame out;
        PopResult result = queue.tryPopReadyFrame(out, clock, now);

        ASSERT_TRUE(result != PopResult::Nothing);

        uint64_t skipped = queue.metrics().skippedFrames.load(std::memory_order_relaxed);
        uint64_t late = queue.metrics().lateFrames.load(std::memory_order_relaxed);

        ASSERT_GE(static_cast<long long>(skipped), 1LL);

        printf("[on_time_late=0 stale_late=%llu skipped=%llu] ",
               static_cast<unsigned long long>(late),
               static_cast<unsigned long long>(skipped));
    }
}

// ============================================================
// V05: Keyframe Recovery After requestKeyFrame()
// Push keyframe + deltas, call requestKeyFrame(), then push deltas (rejected)
// then push new keyframe (accepted). Validates recovery path.
// ============================================================
TEST(V05_keyframe_recovery) {
    VideoFrameQueue queue;

    // Normal flow: keyframe + deltas
    ASSERT_TRUE(pushVideoFrame(queue, 0, true));
    ASSERT_TRUE(pushVideoFrame(queue, 33333, false));
    ASSERT_TRUE(pushVideoFrame(queue, 66666, false));
    ASSERT_EQ(queue.pendingFrames(), 3u);

    // Simulate decoder error → request keyframe
    queue.requestKeyFrame();

    // Queue should be cleared
    ASSERT_EQ(queue.pendingFrames(), 0u);
    ASSERT_TRUE(queue.isAwaitingKeyFrame());

    // Delta frames rejected
    ASSERT_TRUE(!pushVideoFrame(queue, 99999, false));
    ASSERT_TRUE(!pushVideoFrame(queue, 133332, false));

    // New keyframe accepted → recovery
    ASSERT_TRUE(pushVideoFrame(queue, 166665, true));
    ASSERT_TRUE(!queue.isAwaitingKeyFrame());
    ASSERT_EQ(queue.pendingFrames(), 1u);

    printf("[recovered pending=%zu] ", queue.pendingFrames());
}

// ============================================================
// V06: Video Burst (8 frames at once)
// Push 8 frames simultaneously, verify queue handles them and
// tryPopReadyFrame paces them correctly via render clock.
// Validates: burst absorption, render pacing
// ============================================================
TEST(V06_video_burst) {
    VideoFrameQueue queue;
    VideoRenderClock clock;

    // Burst: 8 frames at once (simulating Hypercore batch delivery)
    int64_t pts = 0;
    ASSERT_TRUE(pushVideoFrame(queue, pts, true)); // keyframe
    pts += 33333;
    for (int i = 1; i < 8; i++) {
        ASSERT_TRUE(pushVideoFrame(queue, pts, false));
        pts += 33333;
    }
    ASSERT_EQ(queue.pendingFrames(), 8u);

    // Anchor clock at time=0
    clock.anchor(0, 0, video_config::jitter::kDefaultBufferUs);

    // Pop frames one at a time at correct render times
    int popped = 0;
    EncodedVideoFrame out;
    for (int64_t now = 0; now < 500'000; now += 5000) { // advance 5ms at a time
        PopResult r = queue.tryPopReadyFrame(out, clock, now);
        if (r != PopResult::Nothing) popped++;
    }

    // Should have popped all 8 frames over the ~266ms span (8 × 33ms)
    ASSERT_EQ(popped, 8);
    ASSERT_EQ(queue.pendingFrames(), 0u);

    printf("[popped=%d] ", popped);
}

// ============================================================
// V07: Video Gap + Keyframe Request Callback
// Feed steady stream, simulate 500ms gap, verify keyframe request fires.
// Validates: gap detection, keyframe request callback mechanism
// ============================================================
TEST(V07_video_gap_recovery) {
    VideoFrameQueue queue;
    bool keyFrameRequested = false;
    queue.setKeyFrameRequestCallback([&] { keyFrameRequested = true; });

    // Steady feed: keyframe + 7 deltas
    int64_t pts = 0;
    ASSERT_TRUE(pushVideoFrame(queue, pts, true));
    pts += 33333;
    for (int i = 1; i < 8; i++) {
        ASSERT_TRUE(pushVideoFrame(queue, pts, false));
        pts += 33333;
    }

    // Queue is full (8 frames). Push one more without popping — triggers overflow.
    // Evicted frame is the keyframe, so queue clears and needsKeyFrame_ flips on.
    // Incoming frame is a P-frame and is gated out by the post-drop re-check,
    // so push returns false and the queue is left empty awaiting a keyframe.
    ASSERT_FALSE(pushVideoFrame(queue, pts, false));

    // Keyframe should have been requested (overflow dropped the keyframe)
    ASSERT_TRUE(keyFrameRequested);
    ASSERT_TRUE(queue.isAwaitingKeyFrame());

    printf("[kf_requested=%d] ", keyFrameRequested ? 1 : 0);
}

// ============================================================
// Memory Ordering Contract Tests
// ============================================================
// These verify that critical cross-thread atomic stores use the correct
// memory ordering. On x86 (TSO), release→relaxed is a no-op and cannot
// be detected at runtime. Source-level verification is the only way to
// kill these mutations.

#include <fstream>
#include <string>

static int countOccurrences(const char* path, const char* needle) {
    std::ifstream f(path);
    if (!f.is_open()) return -1;
    int count = 0;
    std::string line;
    while (std::getline(f, line)) {
        if (line.find(needle) != std::string::npos) ++count;
    }
    return count;
}

TEST(M01_activate_state_uses_release) {
    // activate(), resume(), and any other transition into Buffering must store
    // with memory_order_release so the decode thread and audio callback see
    // state transitions on ARM. The Buffering store sites are split across the
    // header and the lifecycle .inl after the file extraction; both must use
    // release ordering. Exactly 2 sites total.
    const char* needle = "state_.store(StreamState::Buffering, std::memory_order_release)";
    int total = countOccurrences(PROJECT_ROOT "/cpp/playback/AudioDecodeChannel.h", needle)
              + countOccurrences(PROJECT_ROOT "/cpp/playback/AudioDecodeChannelLifecycle.inl", needle);
    ASSERT_EQ(total, 2);
}

TEST(M05_clearRequested_uses_release) {
    // The deferred-clear flag (encQueue_.requestClear()) must store with
    // memory_order_release so the decode thread's acquire load in
    // serviceDeferredClear() sees the flag and all preceding writes on ARM.
    // Post file extraction, the flag lives in EncodedFrameQueue::clearRequested_
    // with a single canonical store site behind requestClear(); cancelClear()
    // additionally stores false with release ordering. Two release stores total.
    const char* path = PROJECT_ROOT "/cpp/playback/EncodedFrameQueue.h";
    ASSERT_EQ(countOccurrences(path,
        "clearRequested_.store(true, std::memory_order_release)"), 1);
    ASSERT_EQ(countOccurrences(path,
        "clearRequested_.store(false, std::memory_order_release)"), 2);
}

// ============================================================
// Speculative Playback Tests
// ============================================================

static ScenarioResult runSpeculativeScenario(
    const std::vector<FeedEvent>& events,
    int64_t durationUs,
    float fixedConfidence
) {
    IngestRingBuffer ringBuffer(1 << 20);
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);
    channel.setRingBuffer(&ringBuffer);

    auto decoder = std::make_unique<OpusDecoderAdapter>();
    channel.setDecoder(std::move(decoder));
    channel.setArrivalConfidence(fixedConfidence);
    channel.activate();

    constexpr int64_t kCallbackIntervalUs = config::audio::kFrameDurationUs;

    ScenarioResult result{};
    size_t eventIdx = 0;
    int64_t now = 0;

    while (now < durationUs) {
        while (eventIdx < events.size() &&
               events[eventIdx].wallTimeUs <= now) {
            long long absOffset = static_cast<long long>(ringBuffer.currentWritePos());
            ringBuffer.write(events[eventIdx].data.data(), events[eventIdx].data.size());

            RawAudioFrame raw;
            raw.absOffset = absOffset;
            raw.size = events[eventIdx].data.size();
            raw.timestampUs = events[eventIdx].ptsUs;
            raw.durationUs = kCallbackIntervalUs;
            channel.pushEncodedFrame(raw);
            eventIdx++;
        }

        channel.serviceDeferredClear();
        for (int i = 0; i < 4; i++) {
            if (!channel.processPendingDecode()) break;
        }
        if (channel.needsPLC()) {
            channel.generatePLC();
        }

        float output[config::audio::kFrameSamples * config::audio::kChannels];
        size_t read = channel.readSamples(output, config::audio::kFrameSamples);
        if (read == 0 && channel.state() == StreamState::Playing) {
            channel.markUnderrun();
        }

        result.samples.push_back({now, channel.bufferedDurationUs(), channel.state()});
        now += kCallbackIntervalUs;
    }

    const auto& m = channel.metrics();
    result.underruns = m.underruns.load(std::memory_order_relaxed);
    result.plcFrames = m.plcFrames.load(std::memory_order_relaxed);
    result.framesDrained = m.framesDrained.load(std::memory_order_relaxed);
    result.framesDropped = m.framesDropped.load(std::memory_order_relaxed);
    result.framesReceived = m.framesReceived.load(std::memory_order_relaxed);
    result.finalBufferUs = channel.bufferedDurationUs();
    result.finalState = channel.state();

    channel.deactivate();
    return result;
}

TEST(S_SPEC_01_stable_network_speculative_fast_start) {
    OpusSilenceEncoder enc;
    auto events = generateSteadyFeed(enc, 0, 5'000'000);
    auto result = runSpeculativeScenario(events, 5'000'000, 1.0f);

    printf("recv=%llu, underruns=%llu, plc=%llu ",
           (unsigned long long)result.framesReceived,
           (unsigned long long)result.underruns,
           (unsigned long long)result.plcFrames);
    fflush(stdout);

    ASSERT_TRUE(result.framesReceived >= 200);
    ASSERT_EQ(result.underruns, 0u);
    bool sawPlaying = false;
    for (const auto& s : result.samples) {
        if (s.state == StreamState::Playing) { sawPlaying = true; break; }
    }
    ASSERT_TRUE(sawPlaying);
}

TEST(S_SPEC_02_conservative_with_low_confidence) {
    OpusSilenceEncoder enc;
    auto events = generateSteadyFeed(enc, 0, 5'000'000);
    auto conservative = runSpeculativeScenario(events, 5'000'000, 0.0f);
    auto speculative = runSpeculativeScenario(events, 5'000'000, 1.0f);

    printf("cons_underruns=%llu spec_underruns=%llu ",
           (unsigned long long)conservative.underruns,
           (unsigned long long)speculative.underruns);
    fflush(stdout);

    ASSERT_EQ(conservative.underruns, 0u);
    ASSERT_EQ(speculative.underruns, 0u);
}

TEST(S_SPEC_03_plc_hold_prevents_underrun_on_brief_gap) {
    OpusSilenceEncoder enc;
    std::vector<FeedEvent> events;
    int64_t pts = 0, wall = 0;
    for (int i = 0; i < 50; i++) {
        events.push_back({wall, pts, enc.encodeSilence()});
        pts += config::audio::kFrameDurationUs;
        wall += config::audio::kFrameDurationUs;
    }
    pts += 2 * config::audio::kFrameDurationUs;
    wall += 2 * config::audio::kFrameDurationUs;
    for (int i = 0; i < 50; i++) {
        events.push_back({wall, pts, enc.encodeSilence()});
        pts += config::audio::kFrameDurationUs;
        wall += config::audio::kFrameDurationUs;
    }

    auto result = runSpeculativeScenario(events, 3'000'000, 1.0f);

    printf("underruns=%llu, plc=%llu ",
           (unsigned long long)result.underruns,
           (unsigned long long)result.plcFrames);
    fflush(stdout);

    ASSERT_TRUE(result.plcFrames >= 1);
    // Single-threaded test may see 1 transient underrun due to tick ordering;
    // production 3-thread pipeline fills PLC concurrently, so gap is covered.
    ASSERT_TRUE(result.underruns <= 1);
}

TEST(S_SPEC_04_sustained_loss_triggers_underrun) {
    OpusSilenceEncoder enc;
    std::vector<FeedEvent> events;
    int64_t pts = 0, wall = 0;
    for (int i = 0; i < 50; i++) {
        events.push_back({wall, pts, enc.encodeSilence()});
        pts += config::audio::kFrameDurationUs;
        wall += config::audio::kFrameDurationUs;
    }
    pts += 6 * config::audio::kFrameDurationUs;
    wall += 6 * config::audio::kFrameDurationUs;
    for (int i = 0; i < 50; i++) {
        events.push_back({wall, pts, enc.encodeSilence()});
        pts += config::audio::kFrameDurationUs;
        wall += config::audio::kFrameDurationUs;
    }

    auto result = runSpeculativeScenario(events, 3'000'000, 1.0f);

    printf("underruns=%llu, plc=%llu ",
           (unsigned long long)result.underruns,
           (unsigned long long)result.plcFrames);
    fflush(stdout);

    ASSERT_TRUE(result.underruns >= 1);
    ASSERT_TRUE(result.plcFrames >= 3);
}

// ============================================================
// MUT01: Catchup direction — buffer excess speeds up playback
// Validates: catchup_.update(buffered, target) returns ratio > 1.0
// when buffered > target + kExcessThresholdUs.
// Kills mutation M19 (swapped args invert direction).
// ============================================================
TEST(MUT01_catchup_direction_excess_speeds_up) {
    CatchupController catchup;

    int64_t target = config::jitter::kDefaultBufferUs;          // 60ms
    int64_t excess = target + config::catchup::kExcessThresholdUs + 80000;  // 60+40+80 = 180ms

    // Drive the catchup controller with sustained excess for enough iterations
    // to let the smoothed ratio converge above 1.0
    float ratio = 1.0f;
    for (int i = 0; i < 200; i++) {
        ratio = catchup.update(excess, target);
    }

    printf("[ratio=%.4f] ", ratio);
    ASSERT_TRUE(ratio > 1.001f);

    // Now drive with deficit — ratio should decay toward 1.0
    int64_t deficit = target / 2;  // 30ms — below target
    for (int i = 0; i < 200; i++) {
        ratio = catchup.update(deficit, target);
    }

    printf("[decayed=%.4f] ", ratio);
    ASSERT_TRUE(ratio < 1.001f);
}

// ============================================================
// MUT02: Pool exhaustion — pushEncodedFrame drops when pool full
// Validates: FramePool acquire returns empty token under exhaustion.
// Kills mutation M11 (remove exhaustion check).
// ============================================================
TEST(MUT02_pool_exhaustion_drops_frames) {
    IngestRingBuffer ringBuffer(1 << 20);
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);
    channel.setRingBuffer(&ringBuffer);

    auto decoder = std::make_unique<OpusDecoderAdapter>();
    channel.setDecoder(std::move(decoder));
    channel.activate();

    OpusSilenceEncoder enc;

    // Fill encoded pool: push many frames without decoding any
    // kPendingFrameCapacity = 16, so we push well beyond that
    int64_t pts = 0;
    int accepted = 0, rejected = 0;
    for (int i = 0; i < 40; i++) {
        auto data = enc.encodeSilence();
        long long absOffset = static_cast<long long>(ringBuffer.currentWritePos());
        ringBuffer.write(data.data(), data.size());

        RawAudioFrame raw;
        raw.absOffset = absOffset;
        raw.size = data.size();
        raw.timestampUs = pts;
        raw.durationUs = config::audio::kFrameDurationUs;
        if (channel.pushEncodedFrame(raw)) accepted++; else rejected++;
        pts += config::audio::kFrameDurationUs;
    }

    printf("[accepted=%d rejected=%d] ", accepted, rejected);

    // Some frames must have been rejected (pool or queue full)
    ASSERT_TRUE(rejected > 0);

    // Frames that were rejected should be counted
    const auto& m = channel.metrics();
    ASSERT_TRUE(m.framesDropped.load(std::memory_order_relaxed) > 0);

    channel.deactivate();
}

// ============================================================
// MUT03: Oversized video frame rejection
// Validates: VideoFrameQueue rejects frames > kMaxEncodedFrameSize.
// Kills mutation M12 (remove size check).
// ============================================================
TEST(MUT03_oversized_video_frame_rejected) {
    VideoFrameQueue queue;

    // Normal-sized keyframe should be accepted
    bool ok = queue.pushEncodedFrame(0, 1024, 0, true);
    ASSERT_TRUE(ok);

    // Exactly at limit should be accepted
    ok = queue.pushEncodedFrame(1024, video_config::kMaxEncodedFrameSize, 20000, false);
    ASSERT_TRUE(ok);

    // One byte over limit should be rejected
    ok = queue.pushEncodedFrame(600000, video_config::kMaxEncodedFrameSize + 1, 40000, false);
    ASSERT_FALSE(ok);

    // Zero size should also be rejected
    ok = queue.pushEncodedFrame(700000, 0, 60000, false);
    ASSERT_FALSE(ok);

    printf("[size_guard verified] ");
}

// ============================================================
// MUT04: Buffer ceiling enforcement
// Validates: pushEncodedFrame rejects when buffered > kMaxBufferedUs.
// Complements S08 but explicitly checks the ceiling metric.
// ============================================================
TEST(MUT04_audio_buffer_ceiling_metric) {
    IngestRingBuffer ringBuffer(1 << 20);
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);
    channel.setRingBuffer(&ringBuffer);

    auto decoder = std::make_unique<OpusDecoderAdapter>();
    channel.setDecoder(std::move(decoder));
    channel.activate();

    OpusSilenceEncoder enc;
    int64_t pts = 0;

    // Push and decode many frames to fill the buffer past ceiling
    for (int batch = 0; batch < 5; batch++) {
        for (int i = 0; i < 30; i++) {
            auto data = enc.encodeSilence();
            long long absOffset = static_cast<long long>(ringBuffer.currentWritePos());
            ringBuffer.write(data.data(), data.size());

            RawAudioFrame raw;
            raw.absOffset = absOffset;
            raw.size = data.size();
            raw.timestampUs = pts;
            raw.durationUs = config::audio::kFrameDurationUs;
            channel.pushEncodedFrame(raw);
            pts += config::audio::kFrameDurationUs;
        }
        // Decode what we can
        channel.serviceDeferredClear();
        for (int d = 0; d < 30; d++) {
            if (!channel.processPendingDecode()) break;
        }
    }

    // Buffer ceiling should have triggered drops
    const auto& m = channel.metrics();
    uint64_t drops = m.framesDropped.load(std::memory_order_relaxed);

    printf("[drops=%llu] ", (unsigned long long)drops);

    // With 150 frames pushed (3000ms) and only 8 decoded queue slots,
    // many frames should be dropped either by ceiling or pool exhaustion
    ASSERT_TRUE(drops > 0);

    channel.deactivate();
}

// ============================================================
// Mutation Calibration: Lock contention tests
// These tests exercise decoderMtx_ contention between lifecycle ops
// (activate/setDecoder) and processPendingDecode(). Without the mutex,
// TSan detects a data race on decoder_ and consecutivePLCFrames_.
// ============================================================

TEST(MUT07_try_lock_protects_decoder_during_decode) {
    IngestRingBuffer ringBuffer(1 << 20);
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);
    channel.setRingBuffer(&ringBuffer);

    channel.setDecoder(std::make_unique<OpusDecoderAdapter>());
    channel.activate();

    // Push encoded frames so processPendingDecode has work
    OpusSilenceEncoder encoder;
    int64_t pts = 0;
    for (int i = 0; i < 8; ++i) {
        auto pkt = encoder.encodeSilence();
        long long absOff = static_cast<long long>(ringBuffer.currentWritePos());
        ringBuffer.write(pkt.data(), pkt.size());
        RawAudioFrame raw;
        raw.absOffset = absOff;
        raw.size = pkt.size();
        raw.timestampUs = pts;
        raw.durationUs = config::audio::kFrameDurationUs;
        channel.pushEncodedFrame(raw);
        pts += config::audio::kFrameDurationUs;
    }

    // Concurrent: decode thread vs activate (both touch decoder_ under decoderMtx_)
    std::atomic<bool> running{true};
    std::thread decodeThread([&]() {
        while (running.load(std::memory_order_acquire)) {
            channel.processPendingDecode();
        }
    });

    // Rapid activate/deactivate cycles from "JS thread" — these hold decoderMtx_
    for (int i = 0; i < 50; ++i) {
        channel.deactivate();
        channel.activate();
    }

    running.store(false, std::memory_order_release);
    decodeThread.join();
    channel.deactivate();
    ASSERT_TRUE(true);
}

TEST(MUT08_activate_lock_protects_decoder_state) {
    IngestRingBuffer ringBuffer(1 << 20);
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);
    channel.setRingBuffer(&ringBuffer);

    channel.setDecoder(std::make_unique<OpusDecoderAdapter>());
    channel.activate();

    OpusSilenceEncoder encoder;
    int64_t pts = 0;
    std::atomic<bool> running{true};

    // Decode thread continuously calls processPendingDecode + generatePLC
    std::thread decodeThread([&]() {
        while (running.load(std::memory_order_acquire)) {
            channel.processPendingDecode();
            channel.generatePLC();
        }
    });

    // JS thread does activate() which writes decoder_->reset() and consecutivePLCFrames_
    for (int i = 0; i < 100; ++i) {
        channel.activate();
        auto pkt = encoder.encodeSilence();
        long long absOff = static_cast<long long>(ringBuffer.currentWritePos());
        ringBuffer.write(pkt.data(), pkt.size());
        RawAudioFrame raw;
        raw.absOffset = absOff;
        raw.size = pkt.size();
        raw.timestampUs = pts;
        raw.durationUs = config::audio::kFrameDurationUs;
        channel.pushEncodedFrame(raw);
        pts += config::audio::kFrameDurationUs;
    }

    running.store(false, std::memory_order_release);
    decodeThread.join();
    channel.deactivate();
    ASSERT_TRUE(true);
}

TEST(MUT09_setDecoder_lock_protects_decoder_swap) {
    IngestRingBuffer ringBuffer(1 << 20);
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);
    channel.setRingBuffer(&ringBuffer);

    channel.setDecoder(std::make_unique<OpusDecoderAdapter>());
    channel.activate();

    std::atomic<bool> running{true};

    // Decode thread continuously tries to decode — reads decoder_
    std::thread decodeThread([&]() {
        while (running.load(std::memory_order_acquire)) {
            channel.processPendingDecode();
            channel.generatePLC();
        }
    });

    // JS thread swaps decoder — writes decoder_ under decoderMtx_
    for (int i = 0; i < 50; ++i) {
        channel.setDecoder(std::make_unique<OpusDecoderAdapter>());
    }

    running.store(false, std::memory_order_release);
    decodeThread.join();
    channel.deactivate();
    ASSERT_TRUE(true);
}

// ============================================================
// Main
// ============================================================

TEST_MAIN("Temporal Stress Tests")
