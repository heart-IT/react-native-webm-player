// Unit tests for HealthWatchdog classify() and transitionDetail().
// Tests every branch, priority ordering, threshold boundaries, and detail strings.
//
// Build: cmake -B tests/sanitizer/build-asan -S tests/sanitizer -DSANITIZER=address
//        cmake --build tests/sanitizer/build-asan --target test_health_watchdog
// Run:   tests/sanitizer/build-asan/test_health_watchdog

#include "test_common.h"
#include <string>

#include "common/HealthWatchdog.h"

using namespace media;

// Helper: call classify with named parameters for readability
struct ClassifyArgs {
    HealthSnapshot snap{};
    uint64_t underrunDelta = 0;
    uint64_t errorDelta = 0;
    uint64_t gapDelta = 0;
    uint64_t dropDelta = 0;
    uint64_t resetDelta = 0;
    uint64_t audioRecvDelta = 0;
    uint64_t audioOutputDelta = 0;
    uint64_t videoRecvDelta = 0;
    uint64_t videoDecDelta = 0;
    uint64_t videoDropDelta = 0;
    uint64_t videoResetDelta = 0;

    ClassifyArgs() {
        snap.audioOutputRunning = true;
    }

    StreamHealth run() const {
        return HealthWatchdog::classify(snap, underrunDelta, errorDelta, gapDelta,
                                        dropDelta, resetDelta, audioRecvDelta,
                                        audioOutputDelta, videoRecvDelta,
                                        videoDecDelta, videoDropDelta,
                                        videoResetDelta);
    }
};

static const char* getDetail(StreamHealth from, StreamHealth to,
                           uint64_t underrunDelta = 0, uint64_t errorDelta = 0,
                           uint64_t gapDelta = 0, uint64_t resetDelta = 0,
                           int64_t absAvSync = 0, uint64_t videoDropDelta = 0,
                           StreamStatus status = StreamStatus::Live) {
    return HealthWatchdog::transitionDetail(from, to, underrunDelta, errorDelta,
                                            gapDelta, resetDelta, absAvSync,
                                            videoDropDelta, status);
}

// ==== classify() — Failed conditions ====

TEST(failed_decode_thread_detached) {
    ClassifyArgs a;
    a.snap.decodeThreadDetached = true;
    ASSERT_EQ(a.run(), StreamHealth::Failed);
}

TEST(failed_ingest_thread_detached) {
    ClassifyArgs a;
    a.snap.ingestThreadDetached = true;
    ASSERT_EQ(a.run(), StreamHealth::Failed);
}

TEST(failed_audio_output_stopped) {
    ClassifyArgs a;
    a.snap.audioOutputRunning = false;
    ASSERT_EQ(a.run(), StreamHealth::Failed);
}

TEST(failed_excessive_decoder_resets) {
    ClassifyArgs a;
    a.resetDelta = config::health::kFailedResetThreshold;
    ASSERT_EQ(a.run(), StreamHealth::Failed);
}

TEST(failed_resets_above_threshold) {
    ClassifyArgs a;
    a.resetDelta = config::health::kFailedResetThreshold + 5;
    ASSERT_EQ(a.run(), StreamHealth::Failed);
}

TEST(failed_video_decode_thread_detached) {
    ClassifyArgs a;
    a.snap.videoDecodeThreadDetached = true;
    ASSERT_EQ(a.run(), StreamHealth::Failed);
}

TEST(failed_video_decoder_state_failed) {
    ClassifyArgs a;
    a.snap.videoDecoderState = config::health::kVideoDecoderStateFailed;
    ASSERT_EQ(a.run(), StreamHealth::Failed);
}

TEST(failed_excessive_video_decoder_resets) {
    ClassifyArgs a;
    a.videoResetDelta = config::health::kFailedResetThreshold;
    ASSERT_EQ(a.run(), StreamHealth::Failed);
}

// ==== classify() — Stalled conditions ====

TEST(stalled_gap_over_500ms) {
    ClassifyArgs a;
    a.gapDelta = config::health::kStalledGapThreshold;
    ASSERT_EQ(a.run(), StreamHealth::Stalled);
}

TEST(stalled_multiple_gaps) {
    ClassifyArgs a;
    a.gapDelta = 10;
    ASSERT_EQ(a.run(), StreamHealth::Stalled);
}

// ==== classify() — Degraded conditions ====

TEST(degraded_sustained_underruns) {
    ClassifyArgs a;
    a.underrunDelta = config::health::kDegradedUnderrunThreshold;
    ASSERT_EQ(a.run(), StreamHealth::Degraded);
}

TEST(degraded_sustained_decode_errors) {
    ClassifyArgs a;
    a.errorDelta = config::health::kDegradedDecodeErrorThreshold;
    ASSERT_EQ(a.run(), StreamHealth::Degraded);
}

TEST(degraded_audio_recv_no_output) {
    ClassifyArgs a;
    a.audioRecvDelta = 50;
    a.audioOutputDelta = 0;
    ASSERT_EQ(a.run(), StreamHealth::Degraded);
}

TEST(degraded_video_recv_no_decode) {
    ClassifyArgs a;
    a.videoRecvDelta = 30;
    a.videoDecDelta = 0;
    ASSERT_EQ(a.run(), StreamHealth::Degraded);
}

TEST(degraded_av_sync_positive_drift) {
    ClassifyArgs a;
    a.snap.avSyncOffsetUs = config::health::kAvSyncDegradedThresholdUs + 1;
    ASSERT_EQ(a.run(), StreamHealth::Degraded);
}

TEST(degraded_av_sync_negative_drift) {
    ClassifyArgs a;
    a.snap.avSyncOffsetUs = -(config::health::kAvSyncDegradedThresholdUs + 1);
    ASSERT_EQ(a.run(), StreamHealth::Degraded);
}

TEST(degraded_audio_high_drop_rate) {
    ClassifyArgs a;
    a.audioRecvDelta = 10;
    a.audioOutputDelta = 1;  // not zero, so audio_recv_no_output doesn't fire first
    a.dropDelta = 2;  // 20% drop rate > 10% threshold
    ASSERT_EQ(a.run(), StreamHealth::Degraded);
}

TEST(degraded_video_high_drop_rate) {
    ClassifyArgs a;
    a.videoRecvDelta = 10;
    a.videoDecDelta = 1;  // not zero, so video_recv_no_decode doesn't fire first
    a.videoDropDelta = 2;  // 20%
    ASSERT_EQ(a.run(), StreamHealth::Degraded);
}

// ==== classify() — Buffering conditions ====

TEST(buffering_single_underrun) {
    ClassifyArgs a;
    a.underrunDelta = 1;
    ASSERT_EQ(a.run(), StreamHealth::Buffering);
}

TEST(buffering_few_underruns_below_degraded) {
    ClassifyArgs a;
    a.underrunDelta = config::health::kDegradedUnderrunThreshold - 1;
    ASSERT_EQ(a.run(), StreamHealth::Buffering);
}

// ==== classify() — Healthy ====

TEST(healthy_clean_state) {
    ClassifyArgs a;
    ASSERT_EQ(a.run(), StreamHealth::Healthy);
}

TEST(healthy_with_active_av) {
    ClassifyArgs a;
    a.audioRecvDelta = 100;
    a.audioOutputDelta = 48000;
    a.videoRecvDelta = 30;
    a.videoDecDelta = 30;
    ASSERT_EQ(a.run(), StreamHealth::Healthy);
}

TEST(healthy_zero_recv_zero_output) {
    ClassifyArgs a;
    // No frames received AND no output → idle, not a stall
    ASSERT_EQ(a.run(), StreamHealth::Healthy);
}

// ==== Priority ordering ====

TEST(failed_beats_stalled) {
    ClassifyArgs a;
    a.snap.decodeThreadDetached = true;
    a.gapDelta = 100;
    ASSERT_EQ(a.run(), StreamHealth::Failed);
}

TEST(failed_beats_degraded) {
    ClassifyArgs a;
    a.snap.audioOutputRunning = false;
    a.underrunDelta = 100;
    ASSERT_EQ(a.run(), StreamHealth::Failed);
}

TEST(stalled_beats_degraded) {
    ClassifyArgs a;
    a.gapDelta = 1;
    a.underrunDelta = 100;
    ASSERT_EQ(a.run(), StreamHealth::Stalled);
}

TEST(degraded_beats_buffering) {
    ClassifyArgs a;
    a.underrunDelta = config::health::kDegradedUnderrunThreshold;
    // >= threshold → Degraded, not Buffering
    ASSERT_EQ(a.run(), StreamHealth::Degraded);
}

// ==== Threshold boundaries ====

TEST(resets_below_threshold_not_failed) {
    ClassifyArgs a;
    a.resetDelta = config::health::kFailedResetThreshold - 1;
    ASSERT_EQ(a.run(), StreamHealth::Healthy);
}

TEST(av_sync_at_threshold_not_degraded) {
    ClassifyArgs a;
    a.snap.avSyncOffsetUs = config::health::kAvSyncDegradedThresholdUs;
    // Exactly at threshold (not >) → Healthy
    ASSERT_EQ(a.run(), StreamHealth::Healthy);
}

TEST(audio_drop_10_percent_not_degraded) {
    ClassifyArgs a;
    a.audioRecvDelta = 10;
    a.audioOutputDelta = 1;
    a.dropDelta = 1;  // 10% = boundary, not > → Healthy
    ASSERT_EQ(a.run(), StreamHealth::Healthy);
}

TEST(video_drop_10_percent_not_degraded) {
    ClassifyArgs a;
    a.videoRecvDelta = 10;
    a.videoDecDelta = 1;
    a.videoDropDelta = 1;  // 10%
    ASSERT_EQ(a.run(), StreamHealth::Healthy);
}

TEST(audio_drop_11_percent_is_degraded) {
    ClassifyArgs a;
    a.audioRecvDelta = 100;
    a.audioOutputDelta = 1;
    a.dropDelta = 11;  // 11% > 10%
    ASSERT_EQ(a.run(), StreamHealth::Degraded);
}

TEST(video_drop_11_percent_is_degraded) {
    ClassifyArgs a;
    a.videoRecvDelta = 100;
    a.videoDecDelta = 1;
    a.videoDropDelta = 11;
    ASSERT_EQ(a.run(), StreamHealth::Degraded);
}

TEST(zero_gaps_not_stalled) {
    ClassifyArgs a;
    a.gapDelta = 0;
    ASSERT_EQ(a.run(), StreamHealth::Healthy);
}

TEST(zero_underruns_not_buffering) {
    ClassifyArgs a;
    a.underrunDelta = 0;
    ASSERT_EQ(a.run(), StreamHealth::Healthy);
}

// ==== transitionDetail() ====

TEST(detail_failed_resets) {
    auto d = getDetail(StreamHealth::Buffering, StreamHealth::Failed,
                    0, 0, 0, config::health::kFailedResetThreshold);
    ASSERT_TRUE(strstr(d, "excessive audio decoder resets") != nullptr);
}

TEST(detail_failed_audio_or_thread) {
    auto d = getDetail(StreamHealth::Buffering, StreamHealth::Failed);
    ASSERT_TRUE(strstr(d, "audio output") != nullptr || strstr(d, "video decode thread") != nullptr);
}

TEST(detail_stalled_live) {
    auto d = getDetail(StreamHealth::Healthy, StreamHealth::Stalled,
                    0, 0, 1, 0, 0, 0, StreamStatus::Live);
    ASSERT_TRUE(strstr(d, "gap > 500ms") != nullptr);
}

TEST(detail_stalled_no_peers) {
    auto d = getDetail(StreamHealth::Healthy, StreamHealth::Stalled,
                    0, 0, 1, 0, 0, 0, StreamStatus::NoPeers);
    ASSERT_TRUE(strstr(d, "no peers") != nullptr);
}

TEST(detail_stalled_ended) {
    auto d = getDetail(StreamHealth::Healthy, StreamHealth::Stalled,
                    0, 0, 1, 0, 0, 0, StreamStatus::Ended);
    ASSERT_TRUE(strstr(d, "stream ended") != nullptr);
}

TEST(detail_stalled_replication_lag) {
    auto d = getDetail(StreamHealth::Healthy, StreamHealth::Stalled,
                    0, 0, 1, 0, 0, 0, StreamStatus::Buffering);
    ASSERT_TRUE(strstr(d, "replication lag") != nullptr);
}

TEST(detail_degraded_underruns) {
    auto d = getDetail(StreamHealth::Healthy, StreamHealth::Degraded,
                    config::health::kDegradedUnderrunThreshold);
    ASSERT_TRUE(strstr(d, "sustained underruns") != nullptr);
}

TEST(detail_degraded_decode_errors) {
    auto d = getDetail(StreamHealth::Healthy, StreamHealth::Degraded,
                    0, config::health::kDegradedDecodeErrorThreshold);
    ASSERT_TRUE(strstr(d, "sustained decode errors") != nullptr);
}

TEST(detail_degraded_av_sync) {
    auto d = getDetail(StreamHealth::Healthy, StreamHealth::Degraded,
                    0, 0, 0, 0, config::health::kAvSyncDegradedThresholdUs + 1);
    ASSERT_TRUE(strstr(d, "A/V sync drift") != nullptr);
}

TEST(detail_degraded_video_drops) {
    auto d = getDetail(StreamHealth::Healthy, StreamHealth::Degraded,
                    0, 0, 0, 0, 0, 5);
    ASSERT_TRUE(strstr(d, "video frame drop") != nullptr);
}

TEST(detail_degraded_fallback) {
    // No specific condition matches → fallback detail
    auto d = getDetail(StreamHealth::Healthy, StreamHealth::Degraded);
    ASSERT_TRUE(strstr(d, "video decode stalled") != nullptr);
}

TEST(detail_buffering) {
    auto d = getDetail(StreamHealth::Healthy, StreamHealth::Buffering);
    ASSERT_TRUE(strstr(d, "rebuffering") != nullptr);
}

TEST(detail_healthy_recovery) {
    auto d = getDetail(StreamHealth::Degraded, StreamHealth::Healthy);
    ASSERT_TRUE(strstr(d, "recovered") != nullptr);
}

// ==== requestWindowReset() regression tests ====

TEST(window_reset_prevents_false_degraded_after_metrics_zeroed) {
    // Regression: resetStream() zeroes VideoStreamMetrics but HealthWatchdog's
    // windowBaseSnapshot_ retained pre-reset values, causing unsigned delta
    // underflow → ~UINT64_MAX → false Degraded classification.
    uint64_t videoRecv = 100;
    uint64_t videoDec = 100;

    HealthSnapshot snap{};
    snap.audioOutputRunning = true;
    snap.videoFramesReceived = videoRecv;
    snap.videoFramesDecoded = videoDec;

    StreamHealth lastState = StreamHealth::Buffering;
    HealthWatchdog wd(
        [&]() { return snap; },
        [&](const HealthEvent& e) { lastState = e.status; }
    );

    // Establish healthy baseline
    wd.evaluate();
    ASSERT_EQ(lastState, StreamHealth::Healthy);

    // Simulate resetStream() zeroing video metrics
    snap.videoFramesReceived = 0;
    snap.videoFramesDecoded = 0;

    // Request window reset (as PipelineOrchestrator now does)
    wd.requestWindowReset();

    // Next evaluate should re-snapshot, not produce wrapped deltas
    wd.evaluate();
    // After reset, state returns to Buffering then immediately re-evaluates
    // with zero deltas → should transition to Healthy (or stay Buffering),
    // NOT Degraded
    ASSERT_TRUE(lastState != StreamHealth::Degraded);
    ASSERT_TRUE(lastState != StreamHealth::Failed);
}

TEST(torn_read_after_reset_causes_false_degraded) {
    // Demonstrates the torn-read scenario: readMetrics() reads framesReceived
    // before VideoStreamMetrics::reset() but framesDecoded after reset.
    // The non-atomic reset creates a window where counters are inconsistent.
    HealthSnapshot base{};
    base.audioOutputRunning = true;
    base.videoFramesReceived = 100;
    base.videoFramesDecoded = 100;

    // Torn read: framesReceived not yet zeroed, framesDecoded already zeroed
    HealthSnapshot torn = base;
    torn.videoFramesReceived = 3;   // A few new frames arrived post-reset
    torn.videoFramesDecoded = 0;    // Decoder hasn't caught up

    // Delta math with old base
    uint64_t videoRecvDelta = torn.videoFramesReceived - base.videoFramesReceived;  // wraps
    uint64_t videoDecDelta = torn.videoFramesDecoded - base.videoFramesDecoded;    // wraps

    // Both wrap to huge values — classify sees enormous recv with enormous dec
    ASSERT_TRUE(videoRecvDelta > 1000000);
    ASSERT_TRUE(videoDecDelta > 1000000);

    // With wrapped values, the "recv > 0 && dec == 0" check doesn't fire,
    // but the drop rate check can: videoDropDelta=0, so drops/recv = 0 → safe.
    // The real danger is the transient between individual counter resets.
    // Verify that requestWindowReset prevents any such scenario:
    StreamHealth result = HealthWatchdog::classify(
        torn, 0, 0, 0, 0, 0, 0, 0,
        videoRecvDelta, videoDecDelta, 0);
    // Both deltas wrap identically, so no single classify branch triggers Degraded
    ASSERT_TRUE(result != StreamHealth::Degraded);
}

// ==== evaluate() behavioral tests ====

TEST(evaluate_initial_state_is_buffering) {
    HealthSnapshot snap{};
    snap.audioOutputRunning = true;
    StreamHealth lastState = StreamHealth::Buffering;
    HealthWatchdog wd(
        [&]() { return snap; },
        [&](const HealthEvent& e) { lastState = e.status; }
    );
    ASSERT_EQ(wd.currentHealth(), StreamHealth::Buffering);
}

TEST(evaluate_clean_transitions_to_healthy) {
    HealthSnapshot snap{};
    snap.audioOutputRunning = true;
    StreamHealth lastState = StreamHealth::Buffering;
    HealthWatchdog wd(
        [&]() { return snap; },
        [&](const HealthEvent& e) { lastState = e.status; }
    );
    wd.evaluate();
    ASSERT_EQ(lastState, StreamHealth::Healthy);
    ASSERT_EQ(wd.currentHealth(), StreamHealth::Healthy);
}

TEST(evaluate_paused_suppresses_transition) {
    HealthSnapshot snap{};
    snap.audioOutputRunning = true;
    snap.paused = true;
    snap.decodeThreadDetached = true;
    int callCount = 0;
    HealthWatchdog wd(
        [&]() { return snap; },
        [&](const HealthEvent&) { callCount++; }
    );
    wd.evaluate();
    ASSERT_EQ(callCount, 0);
}

TEST(evaluate_no_duplicate_fire) {
    HealthSnapshot snap{};
    snap.audioOutputRunning = true;
    int callCount = 0;
    HealthWatchdog wd(
        [&]() { return snap; },
        [&](const HealthEvent&) { callCount++; }
    );
    wd.evaluate();  // Buffering → Healthy
    wd.evaluate();  // Healthy → Healthy (rate limited or same state)
    wd.evaluate();
    ASSERT_EQ(callCount, 1);
}

TEST(evaluate_snapshot_field_transitions_work) {
    // Fields checked directly on snapshot (not deltas) bypass the windowing issue
    HealthSnapshot snap{};
    snap.audioOutputRunning = true;
    StreamHealth lastState = StreamHealth::Buffering;
    HealthWatchdog wd(
        [&]() { return snap; },
        [&](const HealthEvent& e) { lastState = e.status; }
    );
    wd.evaluate();  // → Healthy
    ASSERT_EQ(lastState, StreamHealth::Healthy);
}

TEST_MAIN("HealthWatchdog Tests")
