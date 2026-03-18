// LeakSanitizer test for opus-audio C++ components.
// Build: cmake -DSANITIZER=address .. && make
// Run: ./test_lsan
//
// Tests FramePool, FrameToken, FrameQueue,
// Participant, SyncController, DriftCompensator, AudioMixer,
// DecodeThread, OpusDecoderAdapter for memory leaks and use-after-free.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <array>
#include <vector>
#include <memory>
#include <thread>
#include <chrono>

#include "common/MediaConfig.h"
#include "MediaTypes.h"
#include "FramePool.h"
#include "Participant.h"
#include "SyncController.h"
#include "DriftCompensator.h"
#include "OpusDecoderAdapter.h"

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

    // Push several tokens
    for (int i = 0; i < 10; i++) {
        auto tok = pool.acquire();
        ASSERT_TRUE(tok);
        tok->sampleCount = static_cast<uint32_t>(i);
        ASSERT_TRUE(queue.push(std::move(tok)));
    }

    ASSERT_EQ(queue.count(), 10u);

    // Pop all
    for (int i = 0; i < 10; i++) {
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

TEST(frame_queue_clearWithSync_returns_tokens) {
    DecodedAudioPool pool;
    DecodedAudioQueue queue;

    for (int i = 0; i < 5; i++) {
        auto tok = pool.acquire();
        ASSERT_TRUE(tok);
        ASSERT_TRUE(queue.push(std::move(tok)));
    }

    size_t before = pool.available();
    queue.clearWithSync();
    ASSERT_EQ(pool.available(), before + 5);
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
    memset(frame->data.data(), 0xAB, 64);

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
// Participant / ParticipantRegistry tests
// ============================================================

TEST(participant_registry_lifecycle) {
    ParticipantRegistry registry;
    ASSERT_EQ(registry.count(), 0);

    Participant* p = registry.findOrCreate(100);
    ASSERT_TRUE(p != nullptr);
    ASSERT_EQ(registry.count(), 1);

    // Same external ID returns same participant
    Participant* p2 = registry.findOrCreate(100);
    ASSERT_TRUE(p2 == p);
    ASSERT_EQ(registry.count(), 1);

    // Different external ID
    Participant* p3 = registry.findOrCreate(200);
    ASSERT_TRUE(p3 != nullptr);
    ASSERT_TRUE(p3 != p);
    ASSERT_EQ(registry.count(), 2);

    // Remove
    registry.remove(p->id());
    ASSERT_EQ(registry.count(), 1);

    registry.reset();
    ASSERT_EQ(registry.count(), 0);
}

TEST(participant_state_transitions) {
    ParticipantRegistry registry;
    Participant* p = registry.findOrCreate(42);
    ASSERT_TRUE(p != nullptr);

    p->setState(ParticipantState::Connected);
    ASSERT_TRUE(p->isConnected());

    p->setState(ParticipantState::Active);
    ASSERT_TRUE(p->isConnected());
    ASSERT_EQ(p->state(), ParticipantState::Active);

    p->setMuted(true);
    ASSERT_TRUE(p->isMuted());

    p->setAudioSlot(3);
    ASSERT_EQ(p->audioSlot(), 3);
    ASSERT_TRUE(p->hasAudioSlot());

    registry.reset();
}

TEST(participant_registry_full) {
    ParticipantRegistry registry;

    // Fill to capacity
    for (int i = 0; i < ParticipantRegistry::kMaxParticipants; i++) {
        Participant* p = registry.findOrCreate(i + 1000);
        ASSERT_TRUE(p != nullptr);
    }
    ASSERT_EQ(registry.count(), ParticipantRegistry::kMaxParticipants);

    // Registry is full - should fail gracefully
    Participant* overflow = registry.findOrCreate(9999);
    ASSERT_TRUE(overflow == nullptr);

    registry.reset();
    ASSERT_EQ(registry.count(), 0);
}

// ============================================================
// SyncController / JitterEstimator tests
// ============================================================

TEST(sync_controller_jitter_lifecycle) {
    SyncController sync;

    JitterEstimator* j = sync.getJitter(0);
    ASSERT_TRUE(j != nullptr);

    // Feed some packets
    int64_t pts = 0;
    int64_t arrival = 0;
    for (int i = 0; i < 100; i++) {
        pts += 20000;       // 20ms
        arrival += 20000 + (i % 3 == 0 ? 500 : -200); // jitter
        j->onPacket(pts, arrival);
    }

    int64_t target = j->bufferTargetUs();
    ASSERT_TRUE(target >= config::jitter::kMinBufferUs);
    ASSERT_TRUE(target <= config::jitter::kMaxBufferUs);

    sync.release(0);
}

TEST(sync_controller_reset_all) {
    SyncController sync;

    for (int i = 0; i < 10; i++) {
        sync.getJitter(i);
    }

    sync.resetAll();
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
    comp.updateDrift(5000, 30000000, 2000, 0.8f);
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
#include "AudioDecodeSlot.h"

TEST(audio_mixer_lifecycle_no_leak) {
    DecodedAudioPool pool;
    constexpr int kSlots = 4;

    std::array<AudioDecodeSlot, kSlots> slots{
        AudioDecodeSlot(pool), AudioDecodeSlot(pool),
        AudioDecodeSlot(pool), AudioDecodeSlot(pool)
    };
    for (int i = 0; i < kSlots; i++) {
        slots[i].setSlotId(i);
    }

    std::array<AudioDecodeSlot*, AudioMixer::kMaxSlots> slotPtrs{};
    for (int i = 0; i < kSlots; i++) {
        slotPtrs[i] = &slots[i];
    }

    AudioMixer mixer;
    mixer.setSlots(slotPtrs);

    float output[960];
    (void)mixer.mix(output, 960);
}

TEST(audio_mixer_bind_unbind_no_leak) {
    DecodedAudioPool pool;
    AudioDecodeSlot slot(pool);
    slot.setSlotId(0);

    std::array<AudioDecodeSlot*, AudioMixer::kMaxSlots> slotPtrs{};
    slotPtrs[0] = &slot;

    AudioMixer mixer;
    mixer.setSlots(slotPtrs);

    for (int i = 0; i < 20; i++) {
        slot.bind(i);
        float output[960];
        (void)mixer.mix(output, 960);
        slot.unbind();
    }
}

// ============================================================
// DecodeThread: start/stop lifecycle
// ============================================================

#include "DecodeThread.h"

TEST(decode_thread_lifecycle_no_leak) {
    DecodedAudioPool pool;
    constexpr int kSlots = 4;

    std::array<AudioDecodeSlot, kSlots> slots{
        AudioDecodeSlot(pool), AudioDecodeSlot(pool),
        AudioDecodeSlot(pool), AudioDecodeSlot(pool)
    };
    for (int i = 0; i < kSlots; i++) {
        slots[i].setSlotId(i);
    }

    std::array<AudioDecodeSlot*, DecodeThread::kMaxAudioSlots> slotPtrs{};
    for (int i = 0; i < kSlots; i++) {
        slotPtrs[i] = &slots[i];
    }

    DecodeThread dt(slotPtrs);
    ASSERT_TRUE(dt.start());
    ASSERT_TRUE(dt.isRunning());

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    dt.stop();
    ASSERT_TRUE(!dt.isRunning());
}

TEST(decode_thread_rapid_start_stop_no_leak) {
    DecodedAudioPool pool;
    std::array<AudioDecodeSlot*, DecodeThread::kMaxAudioSlots> slotPtrs{};

    for (int i = 0; i < 10; i++) {
        DecodeThread dt(slotPtrs);
        ASSERT_TRUE(dt.start());
        dt.stop();
    }
}

// ============================================================
// Main
// ============================================================

int main() {
    printf("\n=== LeakSanitizer Tests ===\n\n");
    // Tests are auto-registered via static constructors above
    printf("\n--- Results: %d passed, %d failed ---\n\n", g_tests_passed, g_tests_failed);
    return g_tests_failed > 0 ? 1 : 0;
}
