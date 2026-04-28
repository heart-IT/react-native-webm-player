// ThreadSanitizer test for opus-audio C++ components.
// Build: cmake -DSANITIZER=thread .. && make
// Run: ./test_tsan
//
// Tests concurrent access patterns in FramePool, FrameQueue,
// DecodeThread, and AudioDecodeChannel.
#include "test_common.h"
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
#include "JitterEstimator.h"
#include "DriftCompensator.h"
#include "AudioDecodeChannel.h"
#include "AudioMixer.h"
#include "DecodeThread.h"
#include "OpusDecoderAdapter.h"
using namespace media;
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
    // Multiple threads competing for pool entries
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
// JitterEstimator concurrent access
// ============================================================
TEST(jitter_estimator_concurrent_update_read) {
    JitterEstimator jitter;
    jitter.reset();
    constexpr int kPackets = 2000;
    std::atomic<bool> done{false};
    // Packet arrival thread (JS thread in production)
    std::thread arrival([&] {
        int64_t pts = 0;
        int64_t arrival = 0;
        for (int i = 0; i < kPackets; i++) {
            pts += 20000;
            arrival += 20000 + ((i * 7) % 5000 - 2000); // jitter
            jitter.onSample(pts, arrival);
        }
        done.store(true, std::memory_order_release);
    });
    // Buffer target reader (decode thread in production)
    std::thread reader([&] {
        while (!done.load(std::memory_order_acquire)) {
            [[maybe_unused]] int64_t target = jitter.bufferTargetUs();
            [[maybe_unused]] int64_t drift = jitter.estimatedDriftUs();
            [[maybe_unused]] uint32_t samples = jitter.sampleCount();
        }
    });
    arrival.join();
    reader.join();
}
TEST(jitter_estimator_spike_concurrent) {
    JitterEstimator jitter;
    jitter.reset();
    constexpr int kPackets = 2000;
    std::atomic<bool> done{false};
    // Packet arrival thread (JS thread) — injects periodic spikes
    std::thread arrival([&] {
        int64_t pts = 0;
        int64_t arrival = 0;
        for (int i = 0; i < kPackets; i++) {
            pts += 20000;
            // Every 50th packet is a spike (200ms late)
            int64_t jit = (i % 50 == 0) ? 200000 : ((i * 7) % 5000 - 2000);
            arrival += 20000 + jit;
            jitter.onSample(pts, arrival);
        }
        done.store(true, std::memory_order_release);
    });
    // Buffer target reader (decode thread / audio callback)
    std::thread reader([&] {
        while (!done.load(std::memory_order_acquire)) {
            [[maybe_unused]] int64_t target = jitter.bufferTargetUs();
            [[maybe_unused]] int64_t drift = jitter.estimatedDriftUs();
            [[maybe_unused]] uint32_t samples = jitter.sampleCount();
        }
    });
    arrival.join();
    reader.join();
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
            comp.updateDrift(drift * 1000, 30000000, 2000 + i);
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
            drift.updateDrift((i % 200 - 100) * 1000, 30000000, 2000 + i);
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
// AudioDecodeChannel: concurrent push (JS) + read (audio callback)
// This is the core threading pattern of the playback pipeline.
// Uses a mock decoder (no Opus dependency).
// ============================================================
TEST(decode_channel_concurrent_push_read) {
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);
    // Activate channel (no real decoder - we test infrastructure threading)
    channel.activate();
    constexpr int kFrames = 500;
    std::atomic<bool> push_done{false};
    // "JS thread" pushes encoded frames
    std::thread pusher([&] {
        for (int i = 0; i < kFrames; i++) {
            RawAudioFrame raw;
            raw.absOffset = 0;
            raw.size = 64;
            raw.timestampUs = i * 20000LL;
            raw.durationUs = 20000;
            (void)channel.pushEncodedFrame(raw);
            std::this_thread::yield();
        }
        push_done.store(true, std::memory_order_release);
    });
    // "Audio callback" reads state (would read samples if decoder was real)
    std::thread audio_cb([&] {
        float output[960];
        while (!push_done.load(std::memory_order_acquire)) {
            // These reads exercise the lock-free paths
            [[maybe_unused]] bool active = channel.isActive();
            [[maybe_unused]] StreamState s = channel.state();
            [[maybe_unused]] size_t buffered = channel.bufferedFrames();
            [[maybe_unused]] int64_t dur = channel.bufferedDurationUs();
            [[maybe_unused]] float gain = channel.targetGain();
            [[maybe_unused]] bool needsPlc = channel.needsPLC();
            [[maybe_unused]] bool pending = channel.hasPendingDecode();
            // readSamples - will return 0 since no decoder, but exercises the path
            (void)channel.readSamples(output, 960);
            std::this_thread::yield();
        }
    });
    // "Decode thread" processes pending frames
    std::thread decoder([&] {
        while (!push_done.load(std::memory_order_acquire)) {
            (void)channel.processPendingDecode();
            std::this_thread::yield();
        }
        // Drain remaining
        (void)channel.processPendingDecode();
    });
    pusher.join();
    audio_cb.join();
    decoder.join();
    channel.deactivate();
}
TEST(decode_channel_activate_deactivate_concurrent_read) {
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);
    constexpr int kCycles = 200;
    std::atomic<bool> done{false};
    // Rapidly activate/deactivate
    std::thread toggler([&] {
        for (int i = 0; i < kCycles; i++) {
            channel.activate();
            std::this_thread::yield();
            channel.deactivate();
            std::this_thread::yield();
        }
        done.store(true, std::memory_order_release);
    });
    // Concurrent reader (audio callback pattern)
    std::thread reader([&] {
        float output[960];
        while (!done.load(std::memory_order_acquire)) {
            [[maybe_unused]] bool active = channel.isActive();
            [[maybe_unused]] StreamState s = channel.state();
            (void)channel.readSamples(output, 960);
            std::this_thread::yield();
        }
    });
    toggler.join();
    reader.join();
}
// ============================================================
// AudioDecodeChannel: concurrent gain/buffer target updates
// (JS thread writes, audio callback reads)
// ============================================================
TEST(decode_channel_concurrent_gain_updates) {
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);
    channel.activate();
    constexpr int kUpdates = 5000;
    std::atomic<bool> done{false};
    // JS thread updates gain and buffer targets
    std::thread updater([&] {
        for (int i = 0; i < kUpdates; i++) {
            float gain = static_cast<float>(i % 20) / 10.0f;
            channel.setGain(gain);
            int64_t target = 40000 + (i % 10) * 10000;
            channel.setBufferTarget(target);
            // Drift compensation update
            channel.updateDriftCompensation(i * 100, 30000000, 2000 + i);
        }
        done.store(true, std::memory_order_release);
    });
    // Audio callback reads
    std::thread reader([&] {
        while (!done.load(std::memory_order_acquire)) {
            [[maybe_unused]] float gain = channel.targetGain();
            [[maybe_unused]] int64_t target = channel.bufferTarget();
            [[maybe_unused]] int32_t ppm = channel.driftPpm();
            [[maybe_unused]] bool drift = channel.isDriftCompensationActive();
        }
    });
    updater.join();
    reader.join();
    channel.deactivate();
}
// ============================================================
// AudioMixer: concurrent channel reads
// (simulates audio callback reading from channel)
// ============================================================
TEST(audio_mixer_concurrent_access) {
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);
    AudioMixer mixer;
    mixer.setChannel(&channel);
    constexpr int kIterations = 500;
    std::atomic<bool> done{false};
    // "JS thread" activates/deactivates and updates gain
    std::thread js([&] {
        for (int i = 0; i < kIterations; i++) {
            if (i % 5 == 0) {
                channel.activate();
            }
            if (i % 7 == 0) {
                channel.setGain(static_cast<float>(i % 20) / 10.0f);
            }
            if (i % 11 == 0 && channel.isActive()) {
                channel.deactivate();
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
    if (channel.isActive()) channel.deactivate();
}
// ============================================================
// DecodeThread: start/stop + concurrent wake/channel operations
// ============================================================
TEST(decode_thread_start_stop) {
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);
    DecodeThread dt(channel);
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
TEST(decode_thread_concurrent_push) {
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);
    DecodeThread dt(channel);
    (void)dt.start();
    // Activate channel and notify decode thread
    channel.activate();
    dt.notifyActive();
    // Push frames from "JS thread" while decode thread runs
    constexpr int kFrames = 200;
    for (int i = 0; i < kFrames; i++) {
        RawAudioFrame raw;
        raw.absOffset = 0;
        raw.size = 64;
        raw.timestampUs = i * 20000LL;
        raw.durationUs = 20000;
        (void)channel.pushEncodedFrame(raw);
        dt.wake();
    }
    // Let decode thread process
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // Deactivate and stop
    channel.deactivate();
    dt.stop();
}
TEST(decode_thread_watchdog) {
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);
    DecodeThread dt(channel);
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
// Video: VideoFrameQueue concurrent push (JS) + pop (decode thread)
// ============================================================
#include "video/VideoFrameQueue.h"
#include "video/VideoSyncController.h"
TEST(video_queue_concurrent_push_pop) {
    media::VideoFrameQueue queue;
    queue.reset();
    constexpr int kFrames = 200;
    std::atomic<int> pushed{0};
    std::atomic<int> popped{0};
    // Producer: JS thread pushes encoded frames
    std::thread producer([&]() {
        for (int i = 0; i < kFrames; ++i) {
            bool isKey = (i % 15 == 0);
            long long absOffset = static_cast<long long>(i) * 64;
            if (queue.pushEncodedFrame(absOffset, 64,
                                       static_cast<int64_t>(i) * 33333, isKey)) {
                pushed.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });
    // Consumer: decode thread pops frames
    std::thread consumer([&]() {
        media::EncodedVideoFrame frame;
        for (int iter = 0; iter < kFrames * 3; ++iter) {
            if (queue.popEncodedFrame(frame)) {
                popped.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });
    producer.join();
    consumer.join();
    ASSERT_TRUE(pushed.load() > 0);
    // Some frames may be dropped due to queue depth limit — that's fine
    ASSERT_TRUE(popped.load() <= pushed.load());
    ;
}
// ============================================================
// Video: VideoFrameQueue reset during push
// ============================================================
TEST(video_queue_reset_during_push) {
    media::VideoFrameQueue queue;
    std::atomic<bool> stop{false};
    // Writer: pushes frames continuously
    std::thread writer([&]() {
        int seq = 0;
        while (!stop.load(std::memory_order_acquire)) {
            bool isKey = (seq % 15 == 0);
            long long absOffset = static_cast<long long>(seq) * 32;
            queue.pushEncodedFrame(absOffset, 32,
                                   static_cast<int64_t>(seq) * 33333, isKey);
            ++seq;
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });
    // Controller: resets rapidly
    for (int i = 0; i < 20; ++i) {
        queue.reset();
        std::this_thread::sleep_for(std::chrono::microseconds(700));
    }
    stop.store(true, std::memory_order_release);
    writer.join();
}
// ============================================================
// Video: VideoJitterEstimator concurrent update + read
// ============================================================
TEST(video_jitter_concurrent_update_read) {
    media::VideoJitterEstimator jitter;
    std::atomic<bool> stop{false};
    // Writer: simulates frame arrivals
    std::thread writer([&]() {
        int64_t pts = 0;
        int64_t arrivalUs = 1000000;
        int seq = 0;
        while (!stop.load(std::memory_order_acquire)) {
            bool isKey = (seq % 15 == 0);
            jitter.onSample(pts, arrivalUs, isKey);
            pts += 33333;
            arrivalUs += 33333 + (seq % 5) * 1000;
            ++seq;
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });
    // Reader: reads jitter estimates from another thread
    std::thread reader([&]() {
        while (!stop.load(std::memory_order_acquire)) {
            (void)jitter.jitterUs();
            (void)jitter.bufferTargetUs();
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop.store(true, std::memory_order_release);
    writer.join();
    reader.join();
    ASSERT_TRUE(jitter.jitterUs() >= 0);
}
// ============================================================
// Video: VideoSurfaceRegistry concurrent access
// ============================================================
#include "video/VideoSurfaceRegistry.h"
TEST(video_surface_registry_concurrent) {
    auto& reg = media::VideoSurfaceRegistry::instance();
    std::atomic<bool> stop{false};
    // Reader: checks surface from one thread
    std::thread reader([&]() {
        while (!stop.load(std::memory_order_acquire)) {
            (void)reg.hasSurface();
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });
    // Writer: sets resolution from another thread
    std::thread writer([&]() {
        int w = 640, h = 480;
        while (!stop.load(std::memory_order_acquire)) {
            reg.setDecodedResolution(w, h);
            w = (w == 640) ? 1280 : 640;
            h = (h == 480) ? 720 : 480;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop.store(true, std::memory_order_release);
    reader.join();
    writer.join();
}
// ============================================================
// ClipIndex: concurrent ingest + extractClip via shared IngestRingBuffer
// ============================================================
#include "common/ClipIndex.h"
#include "common/IngestRingBuffer.h"

TEST(clip_index_concurrent_ingest_extract) {
    media::IngestRingBuffer ring(65536);  // 64 KB, power of 2
    media::ClipIndex index;
    index.setRingBuffer(&ring);
    index.setEnabled(true);

    std::vector<uint8_t> header(64, 0x1A);
    index.setStreamHeader(header.data(), header.size());

    std::atomic<bool> stop{false};
    constexpr int kChunkSize = 512;

    // Writer thread: writes to ring + updates index metadata
    std::thread writer([&]() {
        std::vector<uint8_t> chunk(kChunkSize, 0xAB);
        int64_t pts = 0;
        int frameCount = 0;
        long long absOffset = 0;
        while (!stop.load(std::memory_order_acquire)) {
            if (!ring.write(chunk.data(), chunk.size())) {
                // Ring full — compact oldest data
                ring.compact(absOffset - static_cast<long long>(ring.capacity() / 2));
                continue;
            }
            if (frameCount % 10 == 0) {
                index.onNewCluster(absOffset);
                index.onKeyFrame(pts);
            }
            index.onBlockPts(pts);
            index.updateRetainPosition();
            absOffset += kChunkSize;
            pts += 20000;
            frameCount++;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    // Reader thread: periodically calls extractClip (takes mutex)
    std::thread reader([&]() {
        while (!stop.load(std::memory_order_acquire)) {
            auto result = index.extractClip(5.0f);
            (void)result.error;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop.store(true, std::memory_order_release);
    writer.join();
    reader.join();
    ASSERT_TRUE(index.availableRangeSeconds() >= 0.0f);
}

TEST(clip_index_extract_from_pts_concurrent) {
    media::IngestRingBuffer ring(32768);  // 32 KB, power of 2
    media::ClipIndex index;
    index.setRingBuffer(&ring);
    index.setEnabled(true);

    std::vector<uint8_t> header(32, 0x1A);
    index.setStreamHeader(header.data(), header.size());

    // Pre-fill some clusters
    std::vector<uint8_t> chunk(256, 0xCD);
    long long absOffset = 0;
    for (int i = 0; i < 50; i++) {
        (void)ring.write(chunk.data(), chunk.size());
        if (i % 5 == 0) {
            index.onNewCluster(absOffset);
            index.onKeyFrame(i * 33000);
        }
        index.onBlockPts(i * 33000);
        absOffset += static_cast<long long>(chunk.size());
    }
    index.updateRetainPosition();

    std::atomic<bool> stop{false};

    // One thread keeps writing to ring + updating index
    std::thread writer([&]() {
        int64_t pts = 50 * 33000;
        while (!stop.load(std::memory_order_acquire)) {
            if (!ring.write(chunk.data(), chunk.size())) {
                ring.compact(absOffset - static_cast<long long>(ring.capacity() / 2));
                continue;
            }
            index.onBlockPts(pts);
            absOffset += static_cast<long long>(chunk.size());
            pts += 33000;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    // Another thread calls extractFromPts
    std::thread reader([&]() {
        while (!stop.load(std::memory_order_acquire)) {
            auto result = index.extractFromPts(100000);
            (void)result.error;
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    stop.store(true, std::memory_order_release);
    writer.join();
    reader.join();
}

// ============================================================
// StallRecoveryController: concurrent evaluate + onDataReceived
// ============================================================
#include "common/StallRecoveryController.h"

TEST(stall_recovery_concurrent_evaluate_data) {
    media::StallRecoveryController ctrl;
    std::atomic<int> keyFrameRequests{0};
    ctrl.setKeyFrameRequestFn([&]() {
        keyFrameRequests.fetch_add(1, std::memory_order_relaxed);
    });
    ctrl.setVideoKeyFrameRequestFn([]() {});

    std::atomic<bool> stop{false};

    // Simulate data arriving on JS thread
    std::thread jsThread([&]() {
        while (!stop.load(std::memory_order_acquire)) {
            ctrl.onDataReceived();
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    });

    // Simulate health evaluation on decode thread
    std::thread decodeThread([&]() {
        while (!stop.load(std::memory_order_acquire)) {
            ctrl.evaluate();
            auto s = ctrl.state();
            (void)s;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop.store(true, std::memory_order_release);
    jsThread.join();
    decodeThread.join();

    auto m = ctrl.metrics();
    ASSERT_TRUE(m.currentState == media::StallState::Healthy ||
                m.currentState == media::StallState::Detecting);
}

TEST(stall_recovery_stall_and_recover) {
    media::StallRecoveryController ctrl;
    std::atomic<int> kfRequests{0};
    ctrl.setKeyFrameRequestFn([&]() {
        kfRequests.fetch_add(1, std::memory_order_relaxed);
    });
    ctrl.setVideoKeyFrameRequestFn([]() {});

    // Send initial data
    ctrl.onDataReceived();

    // Wait for stall threshold
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    ctrl.evaluate();
    ctrl.evaluate();
    ASSERT_TRUE(ctrl.state() == media::StallState::Stalled);
    ASSERT_TRUE(kfRequests.load() > 0);

    // Simulate data resumption
    ctrl.onDataReceived();
    ASSERT_TRUE(ctrl.state() == media::StallState::Recovering);

    // Simulate buffer sufficient
    ctrl.onBufferSufficient();
    ASSERT_TRUE(ctrl.state() == media::StallState::Healthy);

    auto m = ctrl.metrics();
    ASSERT_EQ(m.stallCount, 1u);
    ASSERT_EQ(m.recoveryCount, 1u);
}

// ============================================================
// AudioLevelMeter: concurrent update + read
// ============================================================
#include "AudioMixer.h"

TEST(audio_level_meter_concurrent_update_read) {
    media::AudioLevelMeter meter;
    std::atomic<bool> stop{false};

    // Writer: simulates audio callback updating levels
    std::thread writer([&]() {
        std::array<float, 960> samples{};
        float phase = 0.0f;
        while (!stop.load(std::memory_order_acquire)) {
            for (size_t i = 0; i < samples.size(); i++) {
                samples[i] = 0.5f * std::sin(phase);
                phase += 0.1f;
            }
            meter.update(samples.data(), samples.size());
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    });

    // Reader: simulates metrics collection
    std::thread reader([&]() {
        while (!stop.load(std::memory_order_acquire)) {
            float peak = meter.peakLevel();
            float rms = meter.rmsLevel();
            uint64_t clips = meter.clipCount();
            ASSERT_TRUE(peak >= 0.0f);
            ASSERT_TRUE(rms >= 0.0f);
            (void)clips;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    stop.store(true, std::memory_order_release);
    writer.join();
    reader.join();

    ASSERT_TRUE(meter.peakLevel() > 0.0f);
    ASSERT_TRUE(meter.rmsLevel() > 0.0f);
}

// ============================================================
// SPSC data integrity: verify frames arrive in order with correct data
// ============================================================
TEST(frame_queue_spsc_data_integrity) {
    DecodedAudioPool pool;
    DecodedAudioQueue queue;
    constexpr int kFrames = 500;
    std::atomic<bool> producer_done{false};
    std::atomic<int> consumed{0};
    std::atomic<bool> order_ok{true};

    std::thread producer([&] {
        for (int i = 0; i < kFrames; i++) {
            while (true) {
                auto tok = pool.acquire();
                if (tok) {
                    tok->sampleCount = static_cast<uint32_t>(i);
                    tok->ptsUs = static_cast<int64_t>(i) * 20000;
                    tok->samples[0] = static_cast<float>(i);
                    if (queue.push(std::move(tok))) break;
                }
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        int expected = 0;
        while (expected < kFrames) {
            auto tok = queue.pop();
            if (tok) {
                if (tok->ptsUs != static_cast<int64_t>(expected) * 20000) {
                    order_ok.store(false, std::memory_order_relaxed);
                }
                if (tok->samples[0] != static_cast<float>(expected)) {
                    order_ok.store(false, std::memory_order_relaxed);
                }
                expected++;
            } else if (producer_done.load(std::memory_order_acquire) && queue.empty()) {
                break;
            } else {
                std::this_thread::yield();
            }
        }
        consumed.store(expected, std::memory_order_relaxed);
    });

    producer.join();
    consumer.join();
    ASSERT_EQ(consumed.load(), kFrames);
    ASSERT_TRUE(order_ok.load());
    queue.clear();
    ASSERT_EQ(pool.available(), config::audio::kDecodePoolSize);
}

// ============================================================
// Metrics monotonicity: counters never decrease under concurrent access
// ============================================================
TEST(stream_metrics_monotonic) {
    StreamMetrics metrics;
    constexpr int kUpdates = 10000;
    std::atomic<bool> done{false};
    std::atomic<bool> monotonic{true};

    std::thread writer([&] {
        for (int i = 0; i < kUpdates; i++) {
            metrics.framesReceived.fetch_add(1, std::memory_order_relaxed);
            metrics.samplesOutput.fetch_add(960, std::memory_order_relaxed);
            if (i % 10 == 0) metrics.framesDropped.fetch_add(1, std::memory_order_relaxed);
            if (i % 50 == 0) metrics.underruns.fetch_add(1, std::memory_order_relaxed);
        }
        done.store(true, std::memory_order_release);
    });

    std::thread reader([&] {
        uint64_t prevRecv = 0, prevOut = 0;
        while (!done.load(std::memory_order_acquire)) {
            uint64_t recv = metrics.framesReceived.load(std::memory_order_relaxed);
            uint64_t out = metrics.samplesOutput.load(std::memory_order_relaxed);
            if (recv < prevRecv || out < prevOut) {
                monotonic.store(false, std::memory_order_relaxed);
            }
            prevRecv = recv;
            prevOut = out;
            std::this_thread::yield();
        }
    });

    writer.join();
    reader.join();
    ASSERT_TRUE(monotonic.load());
    ASSERT_EQ(metrics.framesReceived.load(), static_cast<uint64_t>(kUpdates));
    ASSERT_EQ(metrics.samplesOutput.load(), static_cast<uint64_t>(kUpdates) * 960);
}

// ============================================================
// JitterEstimator: concurrent reads produce bounded values
// ============================================================
TEST(jitter_estimator_bounded_output) {
    JitterEstimator jitter;
    jitter.reset();
    constexpr int kPackets = 3000;
    std::atomic<bool> done{false};
    std::atomic<bool> in_bounds{true};

    std::thread arrival([&] {
        int64_t pts = 0;
        int64_t arr = 0;
        for (int i = 0; i < kPackets; i++) {
            pts += 20000;
            arr += 20000 + ((i * 13) % 8000 - 3000);
            jitter.onSample(pts, arr);
        }
        done.store(true, std::memory_order_release);
    });

    std::thread reader([&] {
        while (!done.load(std::memory_order_acquire)) {
            int64_t target = jitter.bufferTargetUs();
            int64_t j = jitter.jitterUs();
            if (target < config::jitter::kMinBufferUs || target > config::jitter::kMaxBufferUs) {
                in_bounds.store(false, std::memory_order_relaxed);
            }
            if (j < 0) {
                in_bounds.store(false, std::memory_order_relaxed);
            }
        }
    });

    arrival.join();
    reader.join();
    ASSERT_TRUE(in_bounds.load());
    // After feeding, buffer target should reflect jitter
    int64_t finalTarget = jitter.bufferTargetUs();
    ASSERT_TRUE(finalTarget >= config::jitter::kMinBufferUs);
    ASSERT_TRUE(finalTarget <= config::jitter::kMaxBufferUs);
}

// ============================================================
// VideoFrameQueue: FIFO integrity — PTS order preserved
// ============================================================
TEST(video_queue_fifo_integrity) {
    media::VideoFrameQueue queue;
    queue.reset();
    constexpr int kFrames = 100;
    std::atomic<bool> producer_done{false};
    std::atomic<bool> order_ok{true};
    std::atomic<int> consumed{0};

    std::thread producer([&]() {
        for (int i = 0; i < kFrames; ++i) {
            bool isKey = (i == 0);
            int64_t pts = static_cast<int64_t>(i) * 33333;
            long long absOffset = static_cast<long long>(i) * 32;
            queue.pushEncodedFrame(absOffset, 32, pts, isKey);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&]() {
        media::EncodedVideoFrame frame;
        int64_t lastPts = -1;
        int count = 0;
        while (count < kFrames) {
            if (queue.popEncodedFrame(frame)) {
                if (frame.ptsUs <= lastPts) {
                    order_ok.store(false, std::memory_order_relaxed);
                }
                int expected_i = static_cast<int>(frame.ptsUs / 33333);
                long long expectedOffset = static_cast<long long>(expected_i) * 32;
                if (frame.absOffset != expectedOffset) {
                    order_ok.store(false, std::memory_order_relaxed);
                }
                lastPts = frame.ptsUs;
                count++;
            } else if (producer_done.load(std::memory_order_acquire)) {
                break;
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
        consumed.store(count, std::memory_order_relaxed);
    });

    producer.join();
    consumer.join();
    ASSERT_TRUE(consumed.load() > 0);
    ASSERT_TRUE(order_ok.load());
}

// ============================================================
// HealthWatchdog: concurrent evaluate + metric updates
// (production pattern: decode thread evaluates while JS feeds data)
// ============================================================
#include "common/HealthWatchdog.h"

TEST(health_watchdog_concurrent_evaluate_metrics) {
    std::atomic<uint64_t> underruns{0};
    std::atomic<uint64_t> errors{0};
    std::atomic<uint64_t> framesRecv{0};
    std::atomic<uint64_t> samplesOut{0};
    std::atomic<bool> audioRunning{true};
    std::atomic<int> transitions{0};

    auto readMetrics = [&]() -> media::HealthSnapshot {
        media::HealthSnapshot snap;
        snap.underruns = underruns.load(std::memory_order_relaxed);
        snap.decodeErrors = errors.load(std::memory_order_relaxed);
        snap.audioFramesReceived = framesRecv.load(std::memory_order_relaxed);
        snap.audioSamplesOutput = samplesOut.load(std::memory_order_relaxed);
        snap.audioOutputRunning = audioRunning.load(std::memory_order_relaxed);
        return snap;
    };

    auto onHealth = [&](const media::HealthEvent& event) {
        transitions.fetch_add(1, std::memory_order_relaxed);
        (void)event;
    };

    media::HealthWatchdog watchdog(readMetrics, onHealth);
    std::atomic<bool> stop{false};

    // Simulate JS thread updating metrics
    std::thread jsThread([&]() {
        for (int i = 0; i < 5000; i++) {
            framesRecv.fetch_add(1, std::memory_order_relaxed);
            samplesOut.fetch_add(960, std::memory_order_relaxed);
            if (i % 200 == 0) underruns.fetch_add(1, std::memory_order_relaxed);
            if (i % 500 == 0) errors.fetch_add(1, std::memory_order_relaxed);
        }
        stop.store(true, std::memory_order_release);
    });

    // Simulate decode thread evaluating health
    std::thread decodeThread([&]() {
        while (!stop.load(std::memory_order_acquire)) {
            watchdog.evaluate();
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        watchdog.evaluate();
    });

    jsThread.join();
    decodeThread.join();

    // Health state should be valid
    auto health = watchdog.currentHealth();
    ASSERT_TRUE(health >= media::StreamHealth::Healthy && health <= media::StreamHealth::Failed);
    auto published = watchdog.publishedHealth();
    ASSERT_TRUE(published >= media::StreamHealth::Healthy && published <= media::StreamHealth::Failed);
}

// ============================================================
// DecodeThread: multi-thread wake (multiple JS sources)
// ============================================================
TEST(decode_thread_multi_wake) {
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);
    DecodeThread dt(channel);
    ASSERT_TRUE(dt.start());

    constexpr int kWakersCount = 4;
    constexpr int kWakesPerThread = 200;
    std::atomic<bool> go{false};
    SpinBarrier barrier(kWakersCount + 1);

    std::vector<std::thread> wakers;
    for (int t = 0; t < kWakersCount; ++t) {
        wakers.emplace_back([&]() {
            barrier.wait();
            for (int i = 0; i < kWakesPerThread; ++i) {
                dt.wake();
            }
        });
    }

    barrier.wait();  // release all wakers simultaneously
    for (auto& w : wakers) w.join();

    // Thread should still be responsive after many concurrent wakes
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(dt.isRunning());
    ASSERT_TRUE(dt.isResponsive());
    dt.stop();
}

// ============================================================
// VideoFrameQueue: concurrent reset + push (stream switch)
// ============================================================
TEST(video_queue_reset_push_integrity) {
    media::VideoFrameQueue queue;
    std::atomic<bool> stop{false};
    std::atomic<int> pushCount{0};
    std::atomic<int> resetCount{0};

    // Simulates JS thread pushing frames
    std::thread pusher([&]() {
        int seq = 0;
        while (!stop.load(std::memory_order_acquire)) {
            bool isKey = (seq % 15 == 0);
            long long absOffset = static_cast<long long>(seq) * 64;
            if (queue.pushEncodedFrame(absOffset, 64,
                                        static_cast<int64_t>(seq) * 33333, isKey)) {
                pushCount.fetch_add(1, std::memory_order_relaxed);
            }
            seq++;
            std::this_thread::yield();
        }
    });

    // Simulates stream switch: periodic reset
    std::thread resetter([&]() {
        for (int i = 0; i < 50; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            queue.reset();
            resetCount.fetch_add(1, std::memory_order_relaxed);
        }
    });

    resetter.join();
    stop.store(true, std::memory_order_release);
    pusher.join();

    ASSERT_TRUE(pushCount.load() > 0);
    ASSERT_EQ(resetCount.load(), 50);
    // Queue should be in a valid state — pending frames count must be non-negative
    // (keyframe state depends on timing of last reset vs last push)
    ASSERT_TRUE(queue.pendingFrames() <= media::video_config::kDecodeQueueDepth);
}

// ============================================================
// VideoStreamMetrics: monotonic under concurrent access
// ============================================================
TEST(video_metrics_monotonic) {
    media::VideoStreamMetrics metrics;
    constexpr int kUpdates = 10000;
    std::atomic<bool> done{false};
    std::atomic<bool> monotonic{true};

    std::thread writer([&] {
        for (int i = 0; i < kUpdates; i++) {
            metrics.framesReceived.fetch_add(1, std::memory_order_relaxed);
            metrics.framesDecoded.fetch_add(1, std::memory_order_relaxed);
            if (i % 20 == 0) metrics.framesDropped.fetch_add(1, std::memory_order_relaxed);
            if (i % 100 == 0) metrics.decodeErrors.fetch_add(1, std::memory_order_relaxed);
        }
        done.store(true, std::memory_order_release);
    });

    std::thread reader([&] {
        uint64_t prevRecv = 0, prevDec = 0;
        while (!done.load(std::memory_order_acquire)) {
            uint64_t recv = metrics.framesReceived.load(std::memory_order_relaxed);
            uint64_t dec = metrics.framesDecoded.load(std::memory_order_relaxed);
            if (recv < prevRecv || dec < prevDec) {
                monotonic.store(false, std::memory_order_relaxed);
            }
            prevRecv = recv;
            prevDec = dec;
            std::this_thread::yield();
        }
    });

    writer.join();
    reader.join();
    ASSERT_TRUE(monotonic.load());
    ASSERT_EQ(metrics.framesReceived.load(), static_cast<uint64_t>(kUpdates));
}

// ============================================================
// M08: activate() lock removal — TSan detects race on decoder_
// and consecutivePLCFrames_ when activate runs without lock
// while processPendingDecode reads them under try_lock.
// ============================================================
TEST(decode_channel_activate_races_decode) {
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);
    IngestRingBuffer ring(1 << 16);  // 64KB
    channel.setRingBuffer(&ring);

    auto decoder = std::make_unique<OpusDecoderAdapter>();
    channel.setDecoder(std::move(decoder));
    channel.activate();

    constexpr int kIterations = 500;
    SpinBarrier barrier(2);
    std::atomic<bool> done{false};

    // Thread 1 (JS thread): rapid activate cycles
    // Each activate() acquires decoderMtx_ and writes decoder_->reset() +
    // consecutivePLCFrames_ = 0. Without the lock (M08), these race.
    std::thread lifecycle([&] {
        barrier.wait();
        for (int i = 0; i < kIterations && !done.load(std::memory_order_acquire); i++) {
            channel.activate();
            std::this_thread::yield();
        }
        done.store(true, std::memory_order_release);
    });

    // Thread 2 (decode thread): processPendingDecode accesses decoder_
    // and consecutivePLCFrames_ under try_lock. Push frames to keep
    // decodeWorkPending_ true so processPendingDecode enters the critical section.
    std::thread decode([&] {
        barrier.wait();
        for (int i = 0; i < kIterations && !done.load(std::memory_order_acquire); i++) {
            // Push a dummy frame descriptor to keep the decode path active
            RawAudioFrame raw;
            raw.absOffset = 0;
            raw.size = 32;
            raw.timestampUs = static_cast<int64_t>(i) * 20000;
            raw.durationUs = 20000;
            (void)channel.pushEncodedFrame(raw);
            (void)channel.processPendingDecode();
            std::this_thread::yield();
        }
    });

    lifecycle.join();
    decode.join();
    channel.deactivate();
}

// ============================================================
// M09: setDecoder() lock removal — TSan detects race on
// decoder_ = std::move(decoder) vs processPendingDecode reading
// decoder_ under try_lock.
// ============================================================
TEST(decode_channel_setDecoder_races_decode) {
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);
    IngestRingBuffer ring(1 << 16);
    channel.setRingBuffer(&ring);

    auto decoder = std::make_unique<OpusDecoderAdapter>();
    channel.setDecoder(std::move(decoder));
    channel.activate();

    constexpr int kIterations = 500;
    SpinBarrier barrier(2);
    std::atomic<bool> done{false};

    // Thread 1 (JS thread): replace decoder repeatedly
    std::thread setter([&] {
        barrier.wait();
        for (int i = 0; i < kIterations && !done.load(std::memory_order_acquire); i++) {
            auto d = std::make_unique<OpusDecoderAdapter>();
            channel.setDecoder(std::move(d));
            std::this_thread::yield();
        }
        done.store(true, std::memory_order_release);
    });

    // Thread 2 (decode thread): processPendingDecode reads decoder_
    std::thread decode([&] {
        barrier.wait();
        for (int i = 0; i < kIterations && !done.load(std::memory_order_acquire); i++) {
            RawAudioFrame raw;
            raw.absOffset = 0;
            raw.size = 32;
            raw.timestampUs = static_cast<int64_t>(i) * 20000;
            raw.durationUs = 20000;
            (void)channel.pushEncodedFrame(raw);
            (void)channel.processPendingDecode();
            std::this_thread::yield();
        }
    });

    setter.join();
    decode.join();
    channel.deactivate();
}

// ============================================================
// M07: try_lock → lock in processPendingDecode — behavioral test.
// With try_lock, processPendingDecode returns immediately when
// mutex is held. With lock (mutation), it blocks.
// ============================================================
#ifdef UNIT_TEST
TEST(decode_channel_try_lock_nonblocking) {
    DecodedAudioPool pool;
    AudioDecodeChannel channel(pool);

    auto decoder = std::make_unique<OpusDecoderAdapter>();
    channel.setDecoder(std::move(decoder));
    channel.activate();

    // Push a frame so processPendingDecode has work to attempt
    RawAudioFrame raw;
    raw.absOffset = 0;
    raw.size = 32;
    raw.timestampUs = 0;
    raw.durationUs = 20000;
    (void)channel.pushEncodedFrame(raw);

    // Hold the mutex from main thread
    auto lock = channel.testLockDecoder();

    std::atomic<int64_t> callDurationUs{0};

    // Worker thread: processPendingDecode should return immediately via try_lock
    std::thread worker([&] {
        auto start = std::chrono::steady_clock::now();
        (void)channel.processPendingDecode();
        auto end = std::chrono::steady_clock::now();
        callDurationUs.store(
            std::chrono::duration_cast<std::chrono::microseconds>(end - start).count(),
            std::memory_order_release);
    });

    // Hold lock for 200ms — if processPendingDecode uses lock (M07 mutation),
    // it will block for this entire duration
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    lock.unlock();

    worker.join();

    int64_t elapsed = callDurationUs.load(std::memory_order_acquire);
    printf("[elapsed=%lldus] ", (long long)elapsed);

    // With try_lock: returns in < 1ms. With lock: blocks ~200ms.
    // 50ms threshold has 50x margin even under TSan slowdown.
    ASSERT_TRUE(elapsed < 50000);

    channel.deactivate();
}
#endif

// ============================================================
// Main
// ============================================================
TEST_MAIN("ThreadSanitizer Tests")
