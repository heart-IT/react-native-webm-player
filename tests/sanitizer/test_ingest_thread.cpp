// Sanitizer tests for IngestRingBuffer (IMkvReader-backed) and IngestThread.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>

#include "common/IngestRingBuffer.h"
#include "common/IngestThread.h"
#include "common/MediaConfig.h"
#include "common/ClipIndex.h"
#include "pipeline/BroadcastPipeline.h"

using namespace media;

static int g_passed = 0, g_failed = 0;

struct TestEntry { const char* name; void (*fn)(); };
static TestEntry g_tests[128];
static int g_testCount = 0;

#define TEST(name) \
    static void test_##name(); \
    static struct TestReg_##name { \
        TestReg_##name() { g_tests[g_testCount++] = {#name, test_##name}; } \
    } g_reg_##name; \
    static void test_##name()

#define ASSERT_TRUE(x) do { if (!(x)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #x); g_failed++; return; } } while(0)
#define ASSERT_FALSE(x) ASSERT_TRUE(!(x))
#define ASSERT_EQ(a, b) do { auto _a = (a); auto _b = (b); if (_a != _b) { printf("FAIL: %s:%d: %s != %s (%zu != %zu)\n", __FILE__, __LINE__, #a, #b, (size_t)_a, (size_t)_b); g_failed++; return; } } while(0)

// ============================================================
// IngestRingBuffer tests (new IMkvReader-based API)
// ============================================================

TEST(ring_basic_write_readAt) {
    IngestRingBuffer ring(1024);
    uint8_t data[] = {1, 2, 3, 4, 5};

    ASSERT_TRUE(ring.write(data, 5));
    ASSERT_EQ(ring.liveBytes(), 5u);

    uint8_t buf[5] = {};
    ASSERT_EQ(ring.readAt(0, 5, buf), 0);
    ASSERT_EQ(buf[0], 1);
    ASSERT_EQ(buf[4], 5);

    ring.compact(5);
    ASSERT_EQ(ring.liveBytes(), 0u);
}

TEST(ring_write_zero_succeeds) {
    IngestRingBuffer ring(1024);
    ASSERT_TRUE(ring.write(nullptr, 0));
    ASSERT_EQ(ring.liveBytes(), 0u);
}

TEST(ring_fill_to_capacity) {
    IngestRingBuffer ring(64);
    std::vector<uint8_t> data(64, 0xAA);

    ASSERT_TRUE(ring.write(data.data(), 64));
    ASSERT_EQ(ring.liveBytes(), 64u);
    ASSERT_EQ(ring.freeSpace(), 0u);

    uint8_t extra = 0xBB;
    ASSERT_FALSE(ring.write(&extra, 1));
}

TEST(ring_wrap_around_readAt) {
    IngestRingBuffer ring(64);

    // Write 48, compact 48 — compactPos now at 48
    std::vector<uint8_t> fill(48, 0xAA);
    ASSERT_TRUE(ring.write(fill.data(), 48));
    ring.compact(48);

    // Write 32 bytes that wrap: 16 at end, 16 at start
    std::vector<uint8_t> data(32);
    for (size_t i = 0; i < 32; i++) data[i] = static_cast<uint8_t>(i);
    ASSERT_TRUE(ring.write(data.data(), 32));
    ASSERT_EQ(ring.liveBytes(), 32u);

    // readAt should handle wrap-around transparently
    uint8_t buf[32] = {};
    ASSERT_EQ(ring.readAt(48, 32, buf), 0);
    for (size_t i = 0; i < 32; i++) {
        ASSERT_EQ(buf[i], static_cast<uint8_t>(i));
    }
}

TEST(ring_dataAt_no_wrap) {
    IngestRingBuffer ring(64);
    uint8_t data[] = {10, 20, 30};
    ASSERT_TRUE(ring.write(data, 3));

    const uint8_t* ptr = ring.dataAt(0, 3);
    ASSERT_TRUE(ptr != nullptr);
    ASSERT_EQ(ptr[0], 10);
    ASSERT_EQ(ptr[2], 30);
}

TEST(ring_dataAt_returns_null_on_wrap) {
    IngestRingBuffer ring(64);

    // Advance past end so next write wraps
    std::vector<uint8_t> fill(60, 0xAA);
    ASSERT_TRUE(ring.write(fill.data(), 60));
    ring.compact(60);

    // Write 10 bytes: 4 at end, 6 at start — wraps
    std::vector<uint8_t> data(10, 0xBB);
    ASSERT_TRUE(ring.write(data.data(), 10));

    // dataAt should return null for wrapping region
    const uint8_t* ptr = ring.dataAt(60, 10);
    ASSERT_TRUE(ptr == nullptr);

    // But readAt should still work (copies into caller's buffer)
    uint8_t buf[10] = {};
    ASSERT_EQ(ring.readAt(60, 10, buf), 0);
    ASSERT_EQ(buf[0], 0xBB);
}

TEST(ring_backpressure) {
    IngestRingBuffer ring(64);
    std::vector<uint8_t> data(48, 0xAA);

    ASSERT_TRUE(ring.write(data.data(), 48));
    std::vector<uint8_t> big(32, 0xBB);
    ASSERT_FALSE(ring.write(big.data(), 32));

    std::vector<uint8_t> exact(16, 0xCC);
    ASSERT_TRUE(ring.write(exact.data(), 16));
    ASSERT_EQ(ring.liveBytes(), 64u);
}

TEST(ring_compact_frees_space) {
    IngestRingBuffer ring(64);
    std::vector<uint8_t> data(64, 0xAA);
    ASSERT_TRUE(ring.write(data.data(), 64));
    ASSERT_EQ(ring.freeSpace(), 0u);

    ring.compact(32);
    ASSERT_EQ(ring.freeSpace(), 32u);
    ASSERT_EQ(ring.liveBytes(), 32u);

    std::vector<uint8_t> more(32, 0xBB);
    ASSERT_TRUE(ring.write(more.data(), 32));
}

TEST(ring_reset) {
    IngestRingBuffer ring(64);
    std::vector<uint8_t> data(32, 0xAA);
    (void)ring.write(data.data(), 32);

    ring.reset();
    ASSERT_EQ(ring.liveBytes(), 0u);
    ASSERT_EQ(ring.freeSpace(), 64u);
}

TEST(ring_multiple_write_compact_cycles) {
    IngestRingBuffer ring(256);

    for (int cycle = 0; cycle < 100; cycle++) {
        uint8_t val = static_cast<uint8_t>(cycle & 0xFF);
        std::vector<uint8_t> data(37, val);
        ASSERT_TRUE(ring.write(data.data(), 37));

        uint8_t buf[37] = {};
        long long absPos = static_cast<long long>(cycle) * 37;
        ASSERT_EQ(ring.readAt(absPos, 37, buf), 0);
        ASSERT_EQ(buf[0], val);

        ring.compact(absPos + 37);
        ASSERT_EQ(ring.liveBytes(), 0u);
    }
}

// ============================================================
// SPSC concurrent test (TSan target)
// ============================================================

TEST(ring_spsc_concurrent) {
    constexpr size_t kCapacity = 4096;
    constexpr size_t kChunkSize = 64;
    constexpr size_t kTotalChunks = 10000;

    IngestRingBuffer ring(kCapacity);
    std::atomic<size_t> bytesVerified{0};

    std::thread producer([&] {
        uint8_t seq = 0;
        for (size_t chunk = 0; chunk < kTotalChunks; chunk++) {
            uint8_t data[kChunkSize];
            for (size_t i = 0; i < kChunkSize; i++) data[i] = seq++;
            while (!ring.write(data, kChunkSize)) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&] {
        uint8_t expectedSeq = 0;
        size_t totalRead = 0;
        size_t target = kTotalChunks * kChunkSize;

        while (totalRead < target) {
            size_t wp = ring.currentWritePos();
            if (wp <= totalRead) { std::this_thread::yield(); continue; }

            size_t avail = wp - totalRead;
            uint8_t buf[256];
            size_t toRead = std::min(avail, sizeof(buf));
            if (ring.readAt(static_cast<long long>(totalRead), static_cast<long>(toRead), buf) != 0) {
                std::this_thread::yield();
                continue;
            }

            for (size_t i = 0; i < toRead; i++) {
                if (buf[i] != expectedSeq) {
                    printf("FAIL: buf[%zu] = %u, expected %u (totalRead=%zu)\n",
                           i, buf[i], expectedSeq, totalRead);
                    g_failed++;
                    return;
                }
                expectedSeq++;
            }
            totalRead += toRead;
            ring.compact(static_cast<long long>(totalRead));
        }
        bytesVerified.store(totalRead, std::memory_order_relaxed);
    });

    producer.join();
    consumer.join();
    ASSERT_EQ(bytesVerified.load(), kTotalChunks * kChunkSize);
}

// ============================================================
// IngestThread lifecycle tests
// ============================================================

// Helper: build a no-op BroadcastPipeline for tests.
static std::unique_ptr<media::pipeline::BroadcastPipeline> makeTestPipeline(
    media::demux::WebmDemuxer& demuxer, media::ClipIndex& clipIndex) {
    return std::make_unique<media::pipeline::BroadcastPipeline>(
        media::pipeline::PipelineConfig{
            .demuxer = demuxer,
            .clipIndex = clipIndex,
            .audioSink = [](const media::demux::AudioPacket*, size_t) {},
            .videoSink = [](const media::demux::VideoPacket*, size_t) {},
            .jitterFloorSink = [](int64_t) {}
        });
}

TEST(ingest_thread_start_stop) {
    IngestRingBuffer ring(4096);
    demux::WebmDemuxer demuxer;
    ClipIndex clipIndex;
    clipIndex.setRingBuffer(&ring);
    auto pipeline = makeTestPipeline(demuxer, clipIndex);

    IngestThread::Deps deps{
        .ringBuffer = ring,
        .pipeline = *pipeline
    };

    IngestThread thread(std::move(deps));
    ASSERT_FALSE(thread.isRunning());
    ASSERT_TRUE(thread.start());
    ASSERT_TRUE(thread.isRunning());
    thread.stop();
    ASSERT_FALSE(thread.isRunning());
    ASSERT_FALSE(thread.wasDetached());
}

TEST(ingest_thread_start_stop_restart) {
    IngestRingBuffer ring(4096);
    demux::WebmDemuxer demuxer;
    ClipIndex clipIndex;
    clipIndex.setRingBuffer(&ring);
    auto pipeline = makeTestPipeline(demuxer, clipIndex);

    IngestThread::Deps deps{
        .ringBuffer = ring,
        .pipeline = *pipeline
    };

    IngestThread thread(std::move(deps));
    for (int i = 0; i < 5; i++) {
        ASSERT_TRUE(thread.start());
        thread.stop();
    }
}

TEST(ingest_thread_pause_resume) {
    IngestRingBuffer ring(4096);
    demux::WebmDemuxer demuxer;
    ClipIndex clipIndex;
    clipIndex.setRingBuffer(&ring);
    auto pipeline = makeTestPipeline(demuxer, clipIndex);

    IngestThread::Deps deps{
        .ringBuffer = ring,
        .pipeline = *pipeline
    };

    IngestThread thread(std::move(deps));
    ASSERT_TRUE(thread.start());

    thread.pause();
    ASSERT_TRUE(thread.isRunning());

    thread.resume();
    ASSERT_TRUE(thread.isRunning());

    thread.stop();
}

TEST(ingest_thread_wake_drains_ring) {
    IngestRingBuffer ring(4096);
    demux::WebmDemuxer demuxer;
    demuxer.setRingBuffer(&ring);
    ClipIndex clipIndex;
    clipIndex.setRingBuffer(&ring);
    auto pipeline = makeTestPipeline(demuxer, clipIndex);

    IngestThread::Deps deps{
        .ringBuffer = ring,
        .pipeline = *pipeline
    };

    IngestThread thread(std::move(deps));
    ASSERT_TRUE(thread.start());

    // Write garbage (not valid WebM, but thread should process without crash)
    std::vector<uint8_t> garbage(256, 0xFF);
    (void)ring.write(garbage.data(), garbage.size());
    thread.wake();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // The ingest thread should have processed the data
    // (demuxer will fail to parse, but no crash)
    ASSERT_TRUE(thread.isResponsive());

    demuxer.setRingBuffer(nullptr);
    thread.stop();
}

// ============================================================

int main() {
    for (int i = 0; i < g_testCount; i++) {
        printf("  %-60s ", g_tests[i].name);
        fflush(stdout);
        int prev = g_failed;
        g_tests[i].fn();
        if (g_failed == prev) {
            printf("PASS\n");
            g_passed++;
        }
    }

    printf("\n=== Ingest Thread Tests ===\n\n");
    printf("  %d passed, %d failed\n\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
