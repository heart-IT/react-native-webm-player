// LeakSanitizer test for opus-audio C++ components.
// Build: cmake -DSANITIZER=address .. && make
// Run: ./test_lsan
//
// Tests FramePool, FrameToken, FrameQueue,
// JitterEstimator, DriftCompensator, AudioMixer,
// DecodeThread, OpusDecoderAdapter for memory leaks and use-after-free.

#include "test_common.h"
#include <cassert>
#include <array>
#include <vector>
#include <memory>
#include <thread>
#include <chrono>

#include "common/MediaConfig.h"
#include "MediaTypes.h"
#include "FramePool.h"
#include "JitterEstimator.h"
#include "DriftCompensator.h"
#include "OpusDecoderAdapter.h"

using namespace media;

// ============================================================
// FramePool tests
// ============================================================

TEST(frame_pool_acquire_release_no_leak) {
    // Acquire all tokens, release them all. LSAN should detect if any leak.
    DecodedAudioPool pool;
    ASSERT_EQ(pool.available(), config::audio::kDecodePoolSize);

    std::vector<DecodedAudioPool::Token> tokens;
    for (size_t i = 0; i < config::audio::kDecodePoolSize; i++) {
        auto tok = pool.acquire();
        ASSERT_TRUE(tok);
        tokens.push_back(std::move(tok));
    }

    ASSERT_EQ(pool.available(), 0u);

    // Pool exhausted
    auto empty = pool.acquire();
    ASSERT_TRUE(!empty);

    // Release all
    tokens.clear();
    ASSERT_EQ(pool.available(), config::audio::kDecodePoolSize);
}

TEST(frame_token_move_no_leak) {
    DecodedAudioPool pool;

    auto tok1 = pool.acquire();
    ASSERT_TRUE(tok1);
    ASSERT_EQ(pool.available(), config::audio::kDecodePoolSize - 1);

    // Move construct
    auto tok2 = std::move(tok1);
    ASSERT_TRUE(!tok1);
    ASSERT_TRUE(tok2);
    ASSERT_EQ(pool.available(), config::audio::kDecodePoolSize - 1);

    // Move assign
    DecodedAudioPool::Token tok3;
    tok3 = std::move(tok2);
    ASSERT_TRUE(!tok2);
    ASSERT_TRUE(tok3);
    ASSERT_EQ(pool.available(), config::audio::kDecodePoolSize - 1);

    // Self-move safety (moved-from state)
    tok3.release();
    ASSERT_EQ(pool.available(), config::audio::kDecodePoolSize);
}

TEST(frame_token_double_release_safe) {
    DecodedAudioPool pool;
    auto tok = pool.acquire();
    ASSERT_TRUE(tok);

    tok.release();
    tok.release(); // Should be no-op, not double-free
    ASSERT_EQ(pool.available(), config::audio::kDecodePoolSize);
}

TEST(frame_token_raii_destructor) {
    DecodedAudioPool pool;
    {
        auto tok = pool.acquire();
        ASSERT_TRUE(tok);
        ASSERT_EQ(pool.available(), config::audio::kDecodePoolSize - 1);
        // tok goes out of scope - RAII release
    }
    ASSERT_EQ(pool.available(), config::audio::kDecodePoolSize);
}

TEST(frame_pool_write_read_data) {
    DecodedAudioPool pool;
    auto tok = pool.acquire();
    ASSERT_TRUE(tok);

    DecodedAudioFrame* frame = tok.get();
    ASSERT_TRUE(frame != nullptr);

    // Write data
    for (int i = 0; i < config::audio::kFrameSize; i++) {
        frame->samples[i] = static_cast<float>(i) / config::audio::kFrameSize;
    }
    frame->sampleCount = config::audio::kFrameSize;
    frame->ptsUs = 12345;

    // Read data back
    ASSERT_EQ(frame->sampleCount, static_cast<uint32_t>(config::audio::kFrameSize));
    ASSERT_EQ(frame->ptsUs, 12345);

    tok.release();
}

// ============================================================
// FrameQueue (SPSC) tests
// ============================================================

TEST(frame_queue_push_pop_no_leak) {
    DecodedAudioPool pool;
    DecodedAudioQueue queue;

    // Push up to queue capacity (kDecodeQueueDepth = 8)
    constexpr size_t kCount = config::audio::kDecodeQueueDepth;
    for (size_t i = 0; i < kCount; i++) {
        auto tok = pool.acquire();
        ASSERT_TRUE(tok);
        tok->sampleCount = static_cast<uint32_t>(i);
        ASSERT_TRUE(queue.push(std::move(tok)));
    }

    ASSERT_EQ(queue.count(), kCount);

    // Pop all
    for (size_t i = 0; i < kCount; i++) {
        auto tok = queue.pop();
        ASSERT_TRUE(tok);
        ASSERT_EQ(tok->sampleCount, static_cast<uint32_t>(i));
    }

    ASSERT_TRUE(queue.empty());
    ASSERT_EQ(pool.available(), config::audio::kDecodePoolSize);
}

TEST(frame_queue_clear_returns_tokens_to_pool) {
    DecodedAudioPool pool;
    DecodedAudioQueue queue;

    for (int i = 0; i < 8; i++) {
        auto tok = pool.acquire();
        ASSERT_TRUE(tok);
        ASSERT_TRUE(queue.push(std::move(tok)));
    }

    size_t before = pool.available();
    queue.clear();
    ASSERT_EQ(pool.available(), before + 8);
}

TEST(frame_queue_full_rejects) {
    DecodedAudioPool pool;
    DecodedAudioQueue queue;

    // Fill queue to capacity
    size_t pushed = 0;
    while (pushed < config::audio::kDecodeQueueDepth) {
        auto tok = pool.acquire();
        if (!tok) break;
        if (!queue.push(std::move(tok))) break;
        pushed++;
    }

    // Queue should be full
    ASSERT_TRUE(queue.full());

    // Next push should fail
    auto tok = pool.acquire();
    if (tok) {
        ASSERT_TRUE(!queue.push(std::move(tok)));
    }

    // Drain everything
    queue.clear();
}

TEST(frame_queue_push_null_token) {
    DecodedAudioQueue queue;
    DecodedAudioPool::Token empty_tok;
    ASSERT_TRUE(!queue.push(std::move(empty_tok)));
}

// ============================================================
// PendingAudioPool/Queue tests (encoded frame path)
// ============================================================

TEST(pending_pool_lifecycle) {
    PendingAudioPool pool;
    ASSERT_EQ(pool.available(), config::audio::kPendingFrameCapacity);

    auto tok = pool.acquire();
    ASSERT_TRUE(tok);

    PendingAudioFrame* frame = tok.get();
    frame->size = 64;
    frame->ptsUs = 999;
    frame->absOffset = 0xAB;

    tok.release();
    ASSERT_EQ(pool.available(), config::audio::kPendingFrameCapacity);
}

TEST(pending_queue_push_pop) {
    PendingAudioPool pool;
    PendingAudioQueue queue;

    for (int i = 0; i < 4; i++) {
        auto tok = pool.acquire();
        ASSERT_TRUE(tok);
        tok->size = static_cast<size_t>(i + 1);
        ASSERT_TRUE(queue.push(std::move(tok)));
    }

    for (int i = 0; i < 4; i++) {
        auto tok = queue.pop();
        ASSERT_TRUE(tok);
        ASSERT_EQ(tok->size, static_cast<size_t>(i + 1));
    }

    ASSERT_EQ(pool.available(), config::audio::kPendingFrameCapacity);
}

// ============================================================
// JitterEstimator tests
// ============================================================

TEST(jitter_estimator_lifecycle) {
    JitterEstimator jitter;
    jitter.reset();

    // Feed some packets
    int64_t pts = 0;
    int64_t arrival = 0;
    for (int i = 0; i < 100; i++) {
        pts += 20000;       // 20ms
        arrival += 20000 + (i % 3 == 0 ? 500 : -200); // jitter
        jitter.onSample(pts, arrival);
    }

    int64_t target = jitter.bufferTargetUs();
    ASSERT_TRUE(target >= config::jitter::kMinBufferUs);
    ASSERT_TRUE(target <= config::jitter::kMaxBufferUs);
}

TEST(jitter_estimator_reset_no_leak) {
    JitterEstimator jitter;

    // Feed packets then reset — no leak
    for (int i = 0; i < 50; i++) {
        jitter.onSample(i * 20000, i * 20000 + 100);
    }

    jitter.reset();
    // No leak, no crash
}

// ============================================================
// DriftCompensator tests
// ============================================================

TEST(drift_compensator_lifecycle) {
    DriftCompensator comp;

    // No drift initially
    float ratio = comp.currentRatio();
    ASSERT_TRUE(std::abs(ratio - 1.0f) < 0.01f);
    ASSERT_TRUE(!comp.isActive());

    // Feed drift data
    comp.updateDrift(5000, 30000000, 2000);
    // Read ratio several times to let smoothing converge
    for (int i = 0; i < 100; i++) {
        ratio = comp.currentRatio();
    }

    comp.reset();
    ratio = comp.currentRatio();
    ASSERT_TRUE(std::abs(ratio - 1.0f) < 0.01f);
}

TEST(speex_drift_resampler_unity) {
    SpeexDriftResampler resampler;
    ASSERT_TRUE(resampler.init());

    float input[960];
    float output[960];
    for (int i = 0; i < 960; i++) {
        input[i] = static_cast<float>(i) / 960.0f;
    }

    // Unity ratio — should pass through
    size_t consumed = 0;
    size_t written = resampler.process(input, 960, output, 960, consumed);

    ASSERT_TRUE(written > 0);
    ASSERT_TRUE(consumed > 0);

    resampler.reset();
    resampler.destroy();
}

TEST(speex_drift_resampler_speedup) {
    SpeexDriftResampler resampler;
    ASSERT_TRUE(resampler.init());

    float input[1024];
    float output[960];
    for (int i = 0; i < 1024; i++) {
        input[i] = static_cast<float>(i) / 1024.0f;
    }

    // 2% speedup
    resampler.setRatio(1.02f);
    size_t consumed = 0;
    size_t written = resampler.process(input, 1024, output, 960, consumed);

    ASSERT_TRUE(written > 0);
    ASSERT_TRUE(consumed > 0);

    resampler.destroy();
}

TEST(catchup_controller_excess) {
    CatchupController catchup;

    // No excess — ratio should be 1.0
    float ratio = catchup.update(60000, 60000);
    ASSERT_TRUE(std::abs(ratio - 1.0f) < 0.01f);

    // 200ms excess above threshold — should speed up
    for (int i = 0; i < 100; i++) {
        ratio = catchup.update(300000, 60000);
    }
    ASSERT_TRUE(ratio > 1.0f);
    ASSERT_TRUE(ratio <= 1.05f);

    catchup.reset();
    ASSERT_TRUE(std::abs(catchup.currentRatio() - 1.0f) < 0.001f);
}

// ============================================================
// Stress test: repeated acquire/release cycles
// ============================================================

TEST(pool_stress_acquire_release_cycles) {
    DecodedAudioPool pool;

    for (int cycle = 0; cycle < 1000; cycle++) {
        std::vector<DecodedAudioPool::Token> tokens;
        int count = (cycle % 16) + 1;
        for (int i = 0; i < count && i < static_cast<int>(config::audio::kDecodePoolSize); i++) {
            auto tok = pool.acquire();
            if (tok) tokens.push_back(std::move(tok));
        }
        // All released on vector destruction
    }

    ASSERT_EQ(pool.available(), config::audio::kDecodePoolSize);
}

// ============================================================
// OpusDecoderAdapter: init/decode/shutdown lifecycle
// ============================================================

TEST(opus_decoder_init_shutdown_no_leak) {
    OpusDecoderAdapter decoder;
    ASSERT_TRUE(decoder.initialize(48000, 1));
    ASSERT_TRUE(decoder.isValid());

    decoder.reset();
    // Destructor releases decoder state — LSAN detects leak
}

TEST(opus_decoder_reinitialize_no_leak) {
    OpusDecoderAdapter decoder;
    ASSERT_TRUE(decoder.initialize(48000, 1));

    // Re-initialize (calls destroy() internally)
    ASSERT_TRUE(decoder.initialize(48000, 1));
    ASSERT_TRUE(decoder.isValid());
}

TEST(opus_decoder_decode_plc) {
    OpusDecoderAdapter decoder;
    ASSERT_TRUE(decoder.initialize(48000, 1));

    std::array<float, 960> output{};

    // PLC (packet loss concealment) — null input
    int result = decoder.decodePLC(output.data(), 960);
    ASSERT_TRUE(result > 0);
}

TEST(opus_decoder_move_no_leak) {
    OpusDecoderAdapter decoder;
    ASSERT_TRUE(decoder.initialize(48000, 1));

    // Move construct
    OpusDecoderAdapter moved(std::move(decoder));
    ASSERT_TRUE(!decoder.isValid());
    ASSERT_TRUE(moved.isValid());

    // Move assign
    OpusDecoderAdapter target;
    target = std::move(moved);
    ASSERT_TRUE(!moved.isValid());
    ASSERT_TRUE(target.isValid());
    // Destructor of target releases decoder state
}

TEST(opus_decoder_multiple_cycles) {
    for (int i = 0; i < 20; ++i) {
        OpusDecoderAdapter decoder;
        ASSERT_TRUE(decoder.initialize(48000, 1));

        std::array<float, 960> output{};
        (void)decoder.decodePLC(output.data(), 960);
    }
}

// ============================================================
// AudioMixer: init/mix/destroy lifecycle
// ============================================================

#include "AudioMixer.h"
#include "AudioDecodeChannel.h"

TEST(audio_mixer_lifecycle_no_leak) {
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);

    AudioMixer mixer;
    mixer.setChannel(&channel);

    float output[960];
    (void)mixer.mix(output, 960);
}

TEST(audio_mixer_activate_deactivate_no_leak) {
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);

    AudioMixer mixer;
    mixer.setChannel(&channel);

    for (int i = 0; i < 20; i++) {
        channel.activate();
        float output[960];
        (void)mixer.mix(output, 960);
        channel.deactivate();
    }
}

// ============================================================
// DecodeThread: start/stop lifecycle
// ============================================================

#include "DecodeThread.h"

TEST(decode_thread_lifecycle_no_leak) {
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);

    DecodeThread dt(channel);
    ASSERT_TRUE(dt.start());
    ASSERT_TRUE(dt.isRunning());

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    dt.stop();
    ASSERT_TRUE(!dt.isRunning());
}

TEST(decode_thread_rapid_start_stop_no_leak) {
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);

    for (int i = 0; i < 10; i++) {
        DecodeThread dt(channel);
        ASSERT_TRUE(dt.start());
        dt.stop();
    }
}

// ============================================================
// ClipIndex: enable/disable/reset cycles should not leak
// ============================================================
#include "common/ClipIndex.h"
#include "common/IngestRingBuffer.h"

TEST(clip_index_enable_disable_no_leak) {
    for (int i = 0; i < 10; i++) {
        media::IngestRingBuffer ring(4096);
        media::ClipIndex index;
        index.setRingBuffer(&ring);
        index.setEnabled(true);

        std::vector<uint8_t> header(32, 0x1A);
        index.setStreamHeader(header.data(), header.size());

        std::vector<uint8_t> chunk(256, 0xAB);
        long long absOffset = 0;
        for (int j = 0; j < 10; j++) {
            (void)ring.write(chunk.data(), chunk.size());
            index.onNewCluster(absOffset);
            index.onKeyFrame(j * 33000);
            index.onBlockPts(j * 33000);

            absOffset += static_cast<long long>(chunk.size());
        }
        index.updateRetainPosition();

        auto result = index.extractClip(2.0f);
        (void)result;

        index.setEnabled(false);
        index.setEnabled(true);
        index.reset();
        // Destructor runs — no leak
    }
}

TEST(clip_index_extract_clip_no_leak) {
    media::IngestRingBuffer ring(8192);
    media::ClipIndex index;
    index.setRingBuffer(&ring);
    index.setEnabled(true);

    std::vector<uint8_t> header(16, 0x1A);
    index.setStreamHeader(header.data(), header.size());

    std::vector<uint8_t> chunk(128, 0xCD);
    long long absOffset = 0;
    for (int i = 0; i < 30; i++) {
        (void)ring.write(chunk.data(), chunk.size());
        index.onNewCluster(absOffset);
        if (i % 3 == 0) index.onKeyFrame(i * 20000);
        index.onBlockPts(i * 20000);
        absOffset += static_cast<long long>(chunk.size());
    }
    index.updateRetainPosition();

    // Extract multiple clips — each returns a vector that must be freed
    for (int i = 0; i < 5; i++) {
        auto result = index.extractClip(1.0f);
        auto result2 = index.extractFromPts(100000);
        (void)result;
        (void)result2;
    }
}

// ============================================================
// StallRecoveryController: no leak on repeated reset
// ============================================================
#include "common/StallRecoveryController.h"

TEST(stall_controller_no_leak) {
    for (int i = 0; i < 10; i++) {
        media::StallRecoveryController ctrl;
        ctrl.setKeyFrameRequestFn([]() {});
        ctrl.setVideoKeyFrameRequestFn([]() {});
        ctrl.onDataReceived();
        ctrl.evaluate();
        ctrl.reset();
    }
}

// ============================================================
// Main
// ============================================================

TEST_MAIN("LeakSanitizer Tests")
