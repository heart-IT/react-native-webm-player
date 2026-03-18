// ThreadSanitizer test for opus-audio C++ components.
// Build: cmake -DSANITIZER=thread .. && make
// Run: ./test_tsan
//
// Tests concurrent access patterns in FramePool, FrameQueue,
// DecodeThread, AudioDecodeSlot, and Participant.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <array>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <functional>

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

// Helper: spin barrier for synchronized thread start
class SpinBarrier {
public:
    explicit SpinBarrier(int count) : count_(count), waiting_(0), phase_(0) {}

    void wait() {
        int p = phase_.load(std::memory_order_acquire);
        if (waiting_.fetch_add(1, std::memory_order_acq_rel) + 1 == count_) {
            waiting_.store(0, std::memory_order_relaxed);
            phase_.fetch_add(1, std::memory_order_release);
        } else {
            while (phase_.load(std::memory_order_acquire) == p) {
                std::this_thread::yield();
            }
        }
    }

private:
    int count_;
    std::atomic<int> waiting_;
    std::atomic<int> phase_;
};

// ============================================================
// FramePool concurrent acquire/release
// ============================================================

TEST(pool_concurrent_acquire_release) {
    // Multiple threads competing for pool slots
    DecodedAudioPool pool;
    constexpr int kThreads = 4;
    constexpr int kOpsPerThread = 500;

    SpinBarrier barrier(kThreads);
    std::atomic<int> total_acquired{0};

    auto worker = [&](int id) {
        barrier.wait();
        for (int i = 0; i < kOpsPerThread; i++) {
            auto tok = pool.acquire();
            if (tok) {
                total_acquired.fetch_add(1, std::memory_order_relaxed);
                // Brief hold then release
                tok->sampleCount = static_cast<uint32_t>(id * 1000 + i);
                tok.release();
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; i++) {
        threads.emplace_back(worker, i);
    }
    for (auto& t : threads) t.join();

    ASSERT_TRUE(total_acquired.load() > 0);
    ASSERT_EQ(pool.available(), config::audio::kDecodePoolSize);
}

TEST(pool_concurrent_acquire_hold_release) {
    // Threads hold tokens for varying durations
    DecodedAudioPool pool;
    constexpr int kThreads = 4;
    constexpr int kOpsPerThread = 200;

    SpinBarrier barrier(kThreads);

    auto worker = [&](int id) {
        barrier.wait();
        for (int i = 0; i < kOpsPerThread; i++) {
            std::vector<DecodedAudioPool::Token> held;
            // Acquire a burst
            int burst = (i % 4) + 1;
            for (int j = 0; j < burst; j++) {
                auto tok = pool.acquire();
                if (tok) held.push_back(std::move(tok));
            }
            // Release all (vector dtor)
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; i++) {
        threads.emplace_back(worker, i);
    }
    for (auto& t : threads) t.join();

    ASSERT_EQ(pool.available(), config::audio::kDecodePoolSize);
}

// ============================================================
// FrameQueue SPSC: producer thread + consumer thread
// ============================================================

TEST(frame_queue_spsc_producer_consumer) {
    DecodedAudioPool pool;
    DecodedAudioQueue queue;
    constexpr int kFrames = 1000;

    std::atomic<bool> producer_done{false};
    std::atomic<int> consumed{0};

    // Producer
    std::thread producer([&] {
        for (int i = 0; i < kFrames; i++) {
            while (true) {
                auto tok = pool.acquire();
                if (tok) {
                    tok->sampleCount = static_cast<uint32_t>(i);
                    tok->ptsUs = i * 20000;
                    if (queue.push(std::move(tok))) break;
                    // Queue full, retry
                }
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    // Consumer
    std::thread consumer([&] {
        int count = 0;
        while (count < kFrames) {
            auto tok = queue.pop();
            if (tok) {
                count++;
            } else if (producer_done.load(std::memory_order_acquire) && queue.empty()) {
                break;
            } else {
                std::this_thread::yield();
            }
        }
        consumed.store(count, std::memory_order_relaxed);
    });

    producer.join();
    consumer.join();

    ASSERT_EQ(consumed.load(), kFrames);
    // All tokens returned
    queue.clear();
    ASSERT_EQ(pool.available(), config::audio::kDecodePoolSize);
}

// ============================================================
// ParticipantRegistry concurrent access
// ============================================================

TEST(participant_registry_concurrent_find_or_create) {
    ParticipantRegistry registry;
    constexpr int kThreads = 4;
    constexpr int kOpsPerThread = 100;

    SpinBarrier barrier(kThreads);

    auto worker = [&](int id) {
        barrier.wait();
        for (int i = 0; i < kOpsPerThread; i++) {
            int externalId = (id * 1000) + (i % 20);
            Participant* p = registry.findOrCreate(externalId);
            if (p) {
                p->setState(ParticipantState::Active);
                p->updateActivity();
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; i++) {
        threads.emplace_back(worker, i);
    }
    for (auto& t : threads) t.join();

    ASSERT_TRUE(registry.count() > 0);
    registry.reset();
}

TEST(participant_concurrent_read_write_atomics) {
    ParticipantRegistry registry;
    Participant* p = registry.findOrCreate(1);
    ASSERT_TRUE(p != nullptr);

    constexpr int kIterations = 10000;
    std::atomic<bool> done{false};

    // Writer thread
    std::thread writer([&] {
        for (int i = 0; i < kIterations; i++) {
            p->setState(i % 2 == 0 ? ParticipantState::Active : ParticipantState::Connected);
            p->setMuted(i % 3 == 0);
            p->setAudioSlot(i % 16);
            p->updateActivity();
        }
        done.store(true, std::memory_order_release);
    });

    // Reader thread
    std::thread reader([&] {
        while (!done.load(std::memory_order_acquire)) {
            [[maybe_unused]] auto s = p->state();
            [[maybe_unused]] bool m = p->isMuted();
            [[maybe_unused]] int slot = p->audioSlot();
            [[maybe_unused]] bool conn = p->isConnected();
            [[maybe_unused]] int64_t act = p->lastActivityUs();
        }
    });

    writer.join();
    reader.join();

    registry.reset();
}

// ============================================================
// SyncController / JitterEstimator concurrent access
// ============================================================

TEST(jitter_estimator_concurrent_update_read) {
    SyncController sync;
    JitterEstimator* j = sync.getJitter(0);
    ASSERT_TRUE(j != nullptr);

    constexpr int kPackets = 2000;
    std::atomic<bool> done{false};

    // Packet arrival thread (JS thread in production)
    std::thread arrival([&] {
        int64_t pts = 0;
        int64_t arrival = 0;
        for (int i = 0; i < kPackets; i++) {
            pts += 20000;
            arrival += 20000 + ((i * 7) % 5000 - 2000); // jitter
            j->onPacket(pts, arrival);
        }
        done.store(true, std::memory_order_release);
    });

    // Buffer target reader (decode thread in production)
    std::thread reader([&] {
        while (!done.load(std::memory_order_acquire)) {
            [[maybe_unused]] int64_t target = j->bufferTargetUs();
            [[maybe_unused]] int64_t drift = j->estimatedDriftUs();
            [[maybe_unused]] uint32_t samples = j->sampleCount();
        }
    });

    arrival.join();
    reader.join();

    sync.release(0);
}

TEST(jitter_estimator_spike_concurrent) {
    SyncController sync;
    JitterEstimator* j = sync.getJitter(0);
    ASSERT_TRUE(j != nullptr);

    constexpr int kPackets = 2000;
    std::atomic<bool> done{false};

    // Packet arrival thread (JS thread) — injects periodic spikes
    std::thread arrival([&] {
        int64_t pts = 0;
        int64_t arrival = 0;
        for (int i = 0; i < kPackets; i++) {
            pts += 20000;
            // Every 50th packet is a spike (200ms late)
            int64_t jitter = (i % 50 == 0) ? 200000 : ((i * 7) % 5000 - 2000);
            arrival += 20000 + jitter;
            j->onPacket(pts, arrival);
        }
        done.store(true, std::memory_order_release);
    });

    // Buffer target reader (decode thread / audio callback)
    std::thread reader([&] {
        while (!done.load(std::memory_order_acquire)) {
            [[maybe_unused]] int64_t target = j->bufferTargetUs();
            [[maybe_unused]] int64_t drift = j->estimatedDriftUs();
            [[maybe_unused]] uint32_t samples = j->sampleCount();
            // Also read the new spike-related state indirectly via bufferTargetUs
        }
    });

    arrival.join();
    reader.join();

    sync.release(0);
}

// ============================================================
// DriftCompensator concurrent update + read
// ============================================================

TEST(drift_compensator_concurrent) {
    DriftCompensator comp;

    constexpr int kUpdates = 1000;
    std::atomic<bool> done{false};

    // JS thread updates drift
    std::thread updater([&] {
        for (int i = 0; i < kUpdates; i++) {
            int64_t drift = (i % 200) - 100; // ±100us
            comp.updateDrift(drift * 1000, 30000000, 2000 + i, 0.5f);
        }
        done.store(true, std::memory_order_release);
    });

    // Audio callback reads ratio
    std::thread reader([&] {
        while (!done.load(std::memory_order_acquire)) {
            [[maybe_unused]] float ratio = comp.currentRatio();
            [[maybe_unused]] bool active = comp.isActive();
            [[maybe_unused]] int32_t ppm = comp.driftPpm();
        }
    });

    updater.join();
    reader.join();
}

TEST(catchup_controller_bidirectional_concurrent) {
    // CatchupController is single-threaded (audio callback only),
    // but test it alongside DriftCompensator to validate the
    // combined ratio path that crosses thread boundaries.
    DriftCompensator drift;
    CatchupController catchup;

    constexpr int kIterations = 1000;
    std::atomic<bool> done{false};

    // JS thread updates drift
    std::thread js([&] {
        for (int i = 0; i < kIterations; i++) {
            drift.updateDrift((i % 200 - 100) * 1000, 30000000, 2000 + i, 0.5f);
        }
        done.store(true, std::memory_order_release);
    });

    // Audio callback: reads drift ratio + updates catchup (speedup + unity)
    std::thread audio([&] {
        while (!done.load(std::memory_order_acquire)) {
            float driftRatio = drift.currentRatio();

            // Alternate between excess buffer (speedup) and deficit (unity)
            int64_t bufferedUs = (rand() % 2 == 0) ? 200000 : 10000;
            float catchupRatio = catchup.update(bufferedUs, 60000);

            // Combined ratio — this is what gets fed to SpeexDriftResampler
            [[maybe_unused]] float combined = driftRatio * catchupRatio;
        }
    });

    js.join();
    audio.join();
}

// ============================================================
// AudioDecodeSlot: concurrent push (JS) + read (audio callback)
// This is the core threading pattern of the playback pipeline.
// Uses a mock decoder (no Opus dependency).
// ============================================================

// Mock OpusDecoderAdapter that produces silence
class MockOpusDecoder : public OpusDecoderAdapter {
    // OpusDecoderAdapter without calling initialize() acts as a null decoder.
    // For this test, we just need the slot infrastructure - no actual decoding.
};

TEST(decode_slot_concurrent_push_read) {
    DecodedAudioPool pool;
    AudioDecodeSlot slot(pool);
    slot.setSlotId(0);

    // Bind a participant (no real decoder - we test infrastructure threading)
    slot.bind(42);

    constexpr int kFrames = 500;
    std::atomic<bool> push_done{false};

    // "JS thread" pushes encoded frames
    std::thread pusher([&] {
        for (int i = 0; i < kFrames; i++) {
            uint8_t data[64] = {};
            data[0] = static_cast<uint8_t>(i & 0xFF);

            RawAudioFrame raw;
            raw.data = data;
            raw.size = sizeof(data);
            raw.timestampUs = i * 20000LL;
            raw.durationUs = 20000;

            (void)slot.pushEncodedFrame(raw);
            std::this_thread::yield();
        }
        push_done.store(true, std::memory_order_release);
    });

    // "Audio callback" reads state (would read samples if decoder was real)
    std::thread audio_cb([&] {
        float output[960];
        while (!push_done.load(std::memory_order_acquire)) {
            // These reads exercise the lock-free paths
            [[maybe_unused]] bool bound = slot.isBound();
            [[maybe_unused]] int pid = slot.boundParticipant();
            [[maybe_unused]] StreamState s = slot.state();
            [[maybe_unused]] size_t buffered = slot.bufferedFrames();
            [[maybe_unused]] int64_t dur = slot.bufferedDurationUs();
            [[maybe_unused]] float gain = slot.targetGain();
            [[maybe_unused]] bool needsPlc = slot.needsPLC();
            [[maybe_unused]] bool pending = slot.hasPendingDecode();

            // readSamples - will return 0 since no decoder, but exercises the path
            (void)slot.readSamples(output, 960);

            std::this_thread::yield();
        }
    });

    // "Decode thread" processes pending frames
    std::thread decoder([&] {
        while (!push_done.load(std::memory_order_acquire)) {
            (void)slot.processPendingDecode();
            std::this_thread::yield();
        }
        // Drain remaining
        (void)slot.processPendingDecode();
    });

    pusher.join();
    audio_cb.join();
    decoder.join();

    slot.unbind();
}

TEST(decode_slot_bind_unbind_concurrent_read) {
    DecodedAudioPool pool;
    AudioDecodeSlot slot(pool);
    slot.setSlotId(1);

    constexpr int kCycles = 200;
    std::atomic<bool> done{false};

    // Rapidly bind/unbind participants
    std::thread binder([&] {
        for (int i = 0; i < kCycles; i++) {
            slot.bind(i % 64);
            std::this_thread::yield();
            slot.unbind();
            std::this_thread::yield();
        }
        done.store(true, std::memory_order_release);
    });

    // Concurrent reader (audio callback pattern)
    std::thread reader([&] {
        float output[960];
        while (!done.load(std::memory_order_acquire)) {
            [[maybe_unused]] int pid = slot.boundParticipant();
            [[maybe_unused]] StreamState s = slot.state();
            (void)slot.readSamples(output, 960);
            std::this_thread::yield();
        }
    });

    binder.join();
    reader.join();
}

// ============================================================
// AudioDecodeSlot: concurrent gain/buffer target updates
// (JS thread writes, audio callback reads)
// ============================================================

TEST(decode_slot_concurrent_gain_updates) {
    DecodedAudioPool pool;
    AudioDecodeSlot slot(pool);
    slot.setSlotId(2);
    slot.bind(10);

    constexpr int kUpdates = 5000;
    std::atomic<bool> done{false};

    // JS thread updates gain and buffer targets
    std::thread updater([&] {
        for (int i = 0; i < kUpdates; i++) {
            float gain = static_cast<float>(i % 20) / 10.0f;
            slot.setGain(gain);

            int64_t target = 40000 + (i % 10) * 10000;
            slot.setBufferTarget(target);

            // Drift compensation update
            slot.updateDriftCompensation(i * 100, 30000000, 2000 + i, 0.7f);
        }
        done.store(true, std::memory_order_release);
    });

    // Audio callback reads
    std::thread reader([&] {
        while (!done.load(std::memory_order_acquire)) {
            [[maybe_unused]] float gain = slot.targetGain();
            [[maybe_unused]] int64_t target = slot.bufferTarget();
            [[maybe_unused]] int32_t ppm = slot.driftPpm();
            [[maybe_unused]] bool drift = slot.isDriftCompensationActive();
        }
    });

    updater.join();
    reader.join();

    slot.unbind();
}

// ============================================================
// AudioMixer: concurrent slot reads
// (simulates audio callback reading from multiple slots)
// ============================================================

TEST(audio_mixer_concurrent_slot_access) {
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

    constexpr int kIterations = 500;
    std::atomic<bool> done{false};

    // "JS thread" binds/unbinds and pushes frames
    std::thread js([&] {
        for (int i = 0; i < kIterations; i++) {
            int slotIdx = i % kSlots;
            if (i % 5 == 0) {
                slots[slotIdx].bind(i % 64);
            }
            if (i % 7 == 0) {
                slots[slotIdx].setGain(static_cast<float>(i % 20) / 10.0f);
            }
            if (i % 11 == 0 && slots[slotIdx].isBound()) {
                slots[slotIdx].unbind();
            }
            std::this_thread::yield();
        }
        done.store(true, std::memory_order_release);
    });

    // "Audio callback" mixes
    std::thread audio([&] {
        float output[960];
        while (!done.load(std::memory_order_acquire)) {
            (void)mixer.mix(output, 960);
            std::this_thread::yield();
        }
    });

    js.join();
    audio.join();

    // Cleanup
    for (int i = 0; i < kSlots; i++) {
        if (slots[i].isBound()) slots[i].unbind();
    }
}

// ============================================================
// DecodeThread: start/stop + concurrent wake/slot operations
// ============================================================

TEST(decode_thread_start_stop) {
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

    // Wake a few times
    for (int i = 0; i < 10; i++) {
        dt.wake();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    dt.stop();
    ASSERT_TRUE(!dt.isRunning());
}

TEST(decode_thread_concurrent_slot_bind_push) {
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
    (void)dt.start();

    // Bind slots and notify decode thread
    for (int i = 0; i < kSlots; i++) {
        slots[i].bind(i);
        dt.notifySlotBound(i);
    }

    // Push frames from "JS thread" while decode thread runs
    constexpr int kFrames = 200;
    for (int i = 0; i < kFrames; i++) {
        int slotIdx = i % kSlots;
        uint8_t data[64] = {};
        data[0] = static_cast<uint8_t>(i);

        RawAudioFrame raw;
        raw.data = data;
        raw.size = sizeof(data);
        raw.timestampUs = i * 20000LL;
        raw.durationUs = 20000;

        (void)slots[slotIdx].pushEncodedFrame(raw);
        dt.wake();
    }

    // Let decode thread process
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Unbind and stop
    for (int i = 0; i < kSlots; i++) {
        dt.notifySlotUnbound(i);
        slots[i].unbind();
    }

    dt.stop();
}

TEST(decode_thread_watchdog) {
    DecodedAudioPool pool;
    std::array<AudioDecodeSlot*, DecodeThread::kMaxAudioSlots> slotPtrs{};

    DecodeThread dt(slotPtrs);
    (void)dt.start();

    // Check watchdog APIs under concurrent execution
    ASSERT_TRUE(dt.isResponsive());
    ASSERT_TRUE(!dt.wasWatchdogTripped());
    ASSERT_TRUE(dt.watchdogTripCount() == 0);
    ASSERT_TRUE(dt.uptimeUs() >= 0);
    ASSERT_TRUE(dt.timeSinceLastHeartbeatUs() >= 0);

    dt.stop();
}

// ============================================================
// Stress: multiple SPSC queues with different types
// ============================================================

TEST(multiple_queues_concurrent) {
    DecodedAudioPool decodedPool;
    PendingAudioPool pendingPool;

    DecodedAudioQueue decodedQueue;
    PendingAudioQueue pendingQueue;

    constexpr int kFrames = 300;
    std::atomic<int> produced1{0};
    std::atomic<bool> prod1_done{false};
    std::atomic<bool> prod2_done{false};

    // Producer for decoded queue
    std::thread prod1([&] {
        for (int i = 0; i < kFrames; i++) {
            while (true) {
                auto tok = decodedPool.acquire();
                if (tok) {
                    tok->sampleCount = static_cast<uint32_t>(i);
                    if (decodedQueue.push(std::move(tok))) {
                        produced1.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                }
                std::this_thread::yield();
            }
        }
        prod1_done.store(true, std::memory_order_release);
    });

    // Consumer for decoded queue
    std::thread cons1([&] {
        while (!prod1_done.load(std::memory_order_acquire) || !decodedQueue.empty()) {
            auto tok = decodedQueue.pop();
            if (!tok) {
                std::this_thread::yield();
            }
        }
    });

    // Producer for pending queue
    std::thread prod2([&] {
        int limit = std::min(kFrames, static_cast<int>(config::audio::kPendingFrameCapacity));
        for (int i = 0; i < limit; i++) {
            while (true) {
                auto tok = pendingPool.acquire();
                if (tok) {
                    tok->size = static_cast<size_t>(i);
                    if (pendingQueue.push(std::move(tok))) break;
                }
                std::this_thread::yield();
            }
        }
        prod2_done.store(true, std::memory_order_release);
    });

    // Consumer for pending queue
    std::thread cons2([&] {
        while (!prod2_done.load(std::memory_order_acquire) || !pendingQueue.empty()) {
            auto tok = pendingQueue.pop();
            if (!tok) std::this_thread::yield();
        }
    });

    prod1.join();
    cons1.join();
    prod2.join();
    cons2.join();

    decodedQueue.clear();
    pendingQueue.clear();
}

// ============================================================
// Metrics concurrent reads (StreamMetrics atomics)
// ============================================================

TEST(stream_metrics_concurrent) {
    StreamMetrics metrics;

    constexpr int kUpdates = 10000;
    std::atomic<bool> done{false};

    std::thread writer([&] {
        for (int i = 0; i < kUpdates; i++) {
            metrics.framesReceived.fetch_add(1, std::memory_order_relaxed);
            metrics.framesDropped.fetch_add(i % 10 == 0 ? 1 : 0, std::memory_order_relaxed);
            metrics.underruns.fetch_add(i % 50 == 0 ? 1 : 0, std::memory_order_relaxed);
            metrics.samplesOutput.fetch_add(960, std::memory_order_relaxed);
        }
        done.store(true, std::memory_order_release);
    });

    std::thread reader([&] {
        while (!done.load(std::memory_order_acquire)) {
            [[maybe_unused]] auto recv = metrics.framesReceived.load(std::memory_order_relaxed);
            [[maybe_unused]] auto drop = metrics.framesDropped.load(std::memory_order_relaxed);
            [[maybe_unused]] auto under = metrics.underruns.load(std::memory_order_relaxed);
            [[maybe_unused]] auto out = metrics.samplesOutput.load(std::memory_order_relaxed);
        }
    });

    writer.join();
    reader.join();
}

// ============================================================
// SyncController concurrent getJitter/release
// ============================================================

TEST(sync_controller_concurrent_get_release) {
    SyncController sync;
    constexpr int kThreads = 4;
    constexpr int kOps = 200;

    SpinBarrier barrier(kThreads);

    auto worker = [&](int id) {
        barrier.wait();
        for (int i = 0; i < kOps; i++) {
            int pid = (id * 10 + i) % 64;
            JitterEstimator* j = sync.getJitter(pid);
            if (j) {
                j->onPacket(i * 20000, i * 20000 + 100);
                [[maybe_unused]] auto t = j->bufferTargetUs();
            }
            if (i % 5 == 0) {
                sync.release(pid);
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; i++) {
        threads.emplace_back(worker, i);
    }
    for (auto& t : threads) t.join();

    sync.resetAll();
}

// ============================================================
// Audio route change threading
// ============================================================

TEST(audio_route_change_concurrent_access) {
    // Simulates the pattern where audio route change notifications arrive
    // on a notification thread while the JS thread accesses participants
    // and route state concurrently.
    std::atomic<int> currentRoute{2};  // Speaker
    std::atomic<int> echoMode{0};      // Hardware AEC
    std::atomic<bool> stop{false};

    ParticipantRegistry registry;

    // Thread 1: simulate route change notifications updating atomics
    std::thread routeThread([&]() {
        int routes[] = {1, 2, 3, 4, 5, 6};  // Earpiece, Speaker, Wired, BT SCO, A2DP, USB
        int echoes[] = {0, 0, 2, 1, 2, 2};  // AEC modes per route
        int idx = 0;
        while (!stop.load(std::memory_order_acquire)) {
            currentRoute.store(routes[idx % 6], std::memory_order_release);
            echoMode.store(echoes[idx % 6], std::memory_order_release);
            idx++;
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });

    // Thread 2: simulate JS thread reading route + managing participants
    std::thread jsThread([&]() {
        for (int i = 0; i < 200; ++i) {
            // Read current route (like getCurrentAudioRoute)
            int route = currentRoute.load(std::memory_order_acquire);
            int echo = echoMode.load(std::memory_order_acquire);
            (void)route;
            (void)echo;

            // Add/remove participants concurrently with route changes
            auto* p = registry.acquireSlot();
            if (p) {
                p->setState(ParticipantState::Connected);
                p->updateActivity();
                registry.remove(p->id());
            }
        }
        stop.store(true, std::memory_order_release);
    });

    routeThread.join();
    jsThread.join();
}

TEST(audio_route_change_with_active_participants) {
    // Test route changes while participants are actively being iterated
    std::atomic<int> currentRoute{2};
    std::atomic<bool> stop{false};

    ParticipantRegistry registry;

    // Pre-add some participants
    for (int i = 0; i < 4; ++i) {
        auto* p = registry.acquireSlot();
        if (p) {
            p->setState(ParticipantState::Active);
            p->updateActivity();
        }
    }

    // Thread 1: iterate participants (like mixer callback reading state)
    std::thread iterThread([&]() {
        while (!stop.load(std::memory_order_acquire)) {
            int count = 0;
            registry.forEach([&](const Participant& p) {
                if (p.state() == ParticipantState::Active && !p.isMuted()) {
                    count++;
                }
            });
            (void)count;
            std::this_thread::sleep_for(std::chrono::microseconds(20));
        }
    });

    // Thread 2: route changes + participant mute toggling
    std::thread routeThread([&]() {
        for (int i = 0; i < 200; ++i) {
            currentRoute.store((i % 2 == 0) ? 2 : 1, std::memory_order_release);

            // Toggle mute on participants during route changes
            registry.forEach([&](Participant& p) {
                p.setMuted(i % 3 == 0);
            });

            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        stop.store(true, std::memory_order_release);
    });

    iterThread.join();
    routeThread.join();
}

// ============================================================
// Main
// ============================================================

int main() {
    printf("\n=== ThreadSanitizer Tests ===\n\n");
    // Tests are auto-registered via static constructors above
    printf("\n--- Results: %d passed, %d failed ---\n\n", g_tests_passed, g_tests_failed);
    return g_tests_failed > 0 ? 1 : 0;
}
