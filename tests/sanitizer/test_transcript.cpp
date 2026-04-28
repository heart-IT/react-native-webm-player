// Transcript module tests: TranscriptRingBuffer, TranscriptRegistry.
//
// Build: cmake -DSANITIZER=address .. && make test_transcript
//        cmake -DSANITIZER=thread  .. && make test_transcript
// Run:   ./test_transcript
//
// Tests:
//   Ring buffer: RT-safe push, consumer read, overflow, data integrity,
//                concurrent producer/consumer, power-of-2 enforcement.
//   Registry:    Thread-safe push/callback, multi-slot callbacks,
//                history bounds, concurrent access.

#include "test_common.h"
#include <cmath>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include <string>

#include "transcript/TranscriptRingBuffer.h"
#include "transcript/TranscriptRegistry.h"

using namespace media::transcript;

// ============================================================
// TranscriptRingBuffer — basic push/read
// ============================================================
TEST(ring_buffer_push_read) {
    TranscriptRingBuffer buf(1024);
    float data[10];
    for (int i = 0; i < 10; i++) data[i] = static_cast<float>(i);

    buf.push(data, 10);
    ASSERT_EQ(buf.available(), 10u);

    float out[10] = {};
    size_t read = buf.read(out, 10);
    ASSERT_EQ(read, 10u);
    for (int i = 0; i < 10; i++) {
        ASSERT_TRUE(out[i] == static_cast<float>(i));
    }
    ASSERT_EQ(buf.available(), 0u);
}

// ============================================================
// TranscriptRingBuffer — read partial
// ============================================================
TEST(ring_buffer_read_partial) {
    TranscriptRingBuffer buf(1024);
    float data[100];
    for (int i = 0; i < 100; i++) data[i] = static_cast<float>(i);

    buf.push(data, 100);

    // Read only 30
    float out[30] = {};
    size_t read = buf.read(out, 30);
    ASSERT_EQ(read, 30u);
    ASSERT_TRUE(out[0] == 0.0f);
    ASSERT_TRUE(out[29] == 29.0f);

    // Read remaining 70
    float out2[70] = {};
    read = buf.read(out2, 70);
    ASSERT_EQ(read, 70u);
    ASSERT_TRUE(out2[0] == 30.0f);
    ASSERT_TRUE(out2[69] == 99.0f);
}

// ============================================================
// TranscriptRingBuffer — wrap around
// ============================================================
TEST(ring_buffer_wrap_around) {
    TranscriptRingBuffer buf(64);  // Small power-of-2
    float data[50];
    for (int i = 0; i < 50; i++) data[i] = static_cast<float>(i);

    // Fill near capacity
    buf.push(data, 50);
    float out[50] = {};
    buf.read(out, 50);

    // Push again — wraps around internal buffer
    for (int i = 0; i < 50; i++) data[i] = static_cast<float>(100 + i);
    buf.push(data, 50);

    float out2[50] = {};
    size_t read = buf.read(out2, 50);
    ASSERT_EQ(read, 50u);
    for (int i = 0; i < 50; i++) {
        ASSERT_TRUE(out2[i] == static_cast<float>(100 + i));
    }
}

// ============================================================
// TranscriptRingBuffer — overflow silently drops oldest
// ============================================================
TEST(ring_buffer_overflow) {
    TranscriptRingBuffer buf(64);
    float data[64];
    for (int i = 0; i < 64; i++) data[i] = static_cast<float>(i);

    // Fill exactly
    buf.push(data, 64);

    // Push 32 more — overwrites oldest 32
    float extra[32];
    for (int i = 0; i < 32; i++) extra[i] = static_cast<float>(100 + i);
    buf.push(extra, 32);

    // Read should report overflow and give us the newest 64 samples
    bool overflowed = false;
    float out[64] = {};
    size_t read = buf.read(out, 64, &overflowed);
    ASSERT_TRUE(overflowed);
    ASSERT_EQ(read, 64u);
    // First 32 should be data[32..63], next 32 should be extra[0..31]
    for (int i = 0; i < 32; i++) {
        ASSERT_TRUE(out[i] == static_cast<float>(32 + i));
    }
    for (int i = 0; i < 32; i++) {
        ASSERT_TRUE(out[32 + i] == static_cast<float>(100 + i));
    }
}

// ============================================================
// TranscriptRingBuffer — overflow flag false when no overflow
// ============================================================
TEST(ring_buffer_no_overflow_flag) {
    TranscriptRingBuffer buf(1024);
    float data[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    buf.push(data, 10);

    bool overflowed = true;  // Pre-set to true
    float out[10] = {};
    buf.read(out, 10, &overflowed);
    ASSERT_TRUE(!overflowed);
}

// ============================================================
// TranscriptRingBuffer — reset clears readable data
// ============================================================
TEST(ring_buffer_reset) {
    TranscriptRingBuffer buf(1024);
    float data[100];
    for (int i = 0; i < 100; i++) data[i] = static_cast<float>(i);
    buf.push(data, 100);
    ASSERT_EQ(buf.available(), 100u);

    buf.reset();
    ASSERT_EQ(buf.available(), 0u);

    float out[10] = {};
    size_t read = buf.read(out, 10);
    ASSERT_EQ(read, 0u);
}

// ============================================================
// TranscriptRingBuffer — concurrent producer/consumer (TSan)
// ============================================================
TEST(ring_buffer_concurrent_push_read) {
    TranscriptRingBuffer buf(8192);  // Power of 2
    constexpr int kChunks = 500;
    constexpr int kChunkSize = 960;  // 20ms at 48kHz
    std::atomic<bool> producerDone{false};
    std::atomic<size_t> totalRead{0};

    // Producer: simulate audio callback pushing 20ms chunks
    std::thread producer([&] {
        float chunk[kChunkSize];
        for (int c = 0; c < kChunks; c++) {
            for (int i = 0; i < kChunkSize; i++) {
                chunk[i] = static_cast<float>(c * kChunkSize + i);
            }
            buf.push(chunk, kChunkSize);
            // Simulate 20ms callback interval (shortened for test speed)
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        producerDone.store(true, std::memory_order_release);
    });

    // Consumer: read in variable-size chunks
    std::thread consumer([&] {
        float out[2048];
        size_t total = 0;
        while (true) {
            size_t read = buf.read(out, 2048);
            total += read;
            if (read == 0) {
                if (producerDone.load(std::memory_order_acquire) && buf.available() == 0) break;
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        }
        totalRead.store(total, std::memory_order_relaxed);
    });

    producer.join();
    consumer.join();

    // May be less than total pushed due to overflow, but should be > 0
    ASSERT_TRUE(totalRead.load() > 0);
}

// ============================================================
// TranscriptRingBuffer — data integrity under contention (TSan)
// ============================================================
TEST(ring_buffer_data_integrity) {
    // Large enough that no overflow occurs
    constexpr size_t kCapacity = 131072;  // 128K, power of 2
    TranscriptRingBuffer buf(kCapacity);
    constexpr int kChunks = 200;
    constexpr int kChunkSize = 480;
    std::atomic<bool> producerDone{false};
    std::atomic<bool> integrityOk{true};

    std::thread producer([&] {
        float chunk[kChunkSize];
        for (int c = 0; c < kChunks; c++) {
            // Write a recognizable pattern: chunk index in every sample
            float val = static_cast<float>(c);
            for (int i = 0; i < kChunkSize; i++) chunk[i] = val;
            buf.push(chunk, kChunkSize);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        producerDone.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        float out[kChunkSize];
        int lastChunk = -1;
        while (true) {
            size_t read = buf.read(out, kChunkSize);
            if (read == kChunkSize) {
                // All samples in a chunk should be the same value
                float expected = out[0];
                for (size_t i = 1; i < read; i++) {
                    if (out[i] != expected) {
                        integrityOk.store(false, std::memory_order_relaxed);
                        return;
                    }
                }
                // Chunks should arrive in order (no reordering)
                int chunkIdx = static_cast<int>(expected);
                if (lastChunk >= 0 && chunkIdx <= lastChunk) {
                    integrityOk.store(false, std::memory_order_relaxed);
                    return;
                }
                lastChunk = chunkIdx;
            } else if (read == 0) {
                if (producerDone.load(std::memory_order_acquire) && buf.available() == 0) break;
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            // Partial reads are fine — just continue
        }
    });

    producer.join();
    consumer.join();
    ASSERT_TRUE(integrityOk.load());
}

// ============================================================
// TranscriptRegistry — single slot callback fires
// ============================================================
TEST(registry_callback_fires) {
    auto& reg = TranscriptRegistry::instance();
    reg.clearHistory();

    std::atomic<int> callCount{0};
    reg.setCallback(CallbackSlot::NativeView, [&](const TranscriptSegment&) {
        callCount.fetch_add(1, std::memory_order_relaxed);
    });

    TranscriptSegment seg;
    seg.text = "hello world";
    seg.startUs = 1000;
    seg.endUs = 2000;
    seg.isFinal = true;
    reg.pushSegment(seg);

    ASSERT_EQ(callCount.load(), 1);

    reg.clearCallback(CallbackSlot::NativeView);
    reg.clearHistory();
}

// ============================================================
// TranscriptRegistry — both slots fire independently
// ============================================================
TEST(registry_dual_callbacks) {
    auto& reg = TranscriptRegistry::instance();
    reg.clearHistory();

    std::atomic<int> nativeCount{0};
    std::atomic<int> jsCount{0};

    reg.setCallback(CallbackSlot::NativeView, [&](const TranscriptSegment&) {
        nativeCount.fetch_add(1, std::memory_order_relaxed);
    });
    reg.setCallback(CallbackSlot::JsCallback, [&](const TranscriptSegment&) {
        jsCount.fetch_add(1, std::memory_order_relaxed);
    });

    TranscriptSegment seg;
    seg.text = "test";
    reg.pushSegment(seg);
    reg.pushSegment(seg);
    reg.pushSegment(seg);

    ASSERT_EQ(nativeCount.load(), 3);
    ASSERT_EQ(jsCount.load(), 3);

    // Clear one — other still fires
    reg.clearCallback(CallbackSlot::NativeView);
    reg.pushSegment(seg);
    ASSERT_EQ(nativeCount.load(), 3);  // unchanged
    ASSERT_EQ(jsCount.load(), 4);

    reg.clearCallback(CallbackSlot::JsCallback);
    reg.clearHistory();
}

// ============================================================
// TranscriptRegistry — history bounded at 200
// ============================================================
TEST(registry_history_bounded) {
    auto& reg = TranscriptRegistry::instance();
    reg.clearHistory();

    for (int i = 0; i < 250; i++) {
        TranscriptSegment seg;
        seg.text = "seg" + std::to_string(i);
        seg.startUs = i * 1000;
        reg.pushSegment(seg);
    }

    auto history = reg.getHistory();
    ASSERT_EQ(history.size(), 200u);
    // Oldest should be seg50 (first 50 evicted)
    ASSERT_TRUE(history[0].text == "seg50");
    ASSERT_TRUE(history[199].text == "seg249");

    reg.clearHistory();
}

// ============================================================
// TranscriptRegistry — concurrent push + read (TSan)
// ============================================================
TEST(registry_concurrent_push_read) {
    auto& reg = TranscriptRegistry::instance();
    reg.clearHistory();

    constexpr int kSegments = 300;
    std::atomic<int> cbCount{0};

    reg.setCallback(CallbackSlot::NativeView, [&](const TranscriptSegment&) {
        cbCount.fetch_add(1, std::memory_order_relaxed);
    });

    // Producer: push segments from a background thread
    std::thread producer([&] {
        for (int i = 0; i < kSegments; i++) {
            TranscriptSegment seg;
            seg.text = "word" + std::to_string(i);
            seg.startUs = i * 2000;
            seg.endUs = seg.startUs + 1500;
            seg.isFinal = true;
            reg.pushSegment(seg);
        }
    });

    // Reader: poll history from main thread
    std::thread reader([&] {
        for (int i = 0; i < 100; i++) {
            auto h = reg.getHistory();
            // Just exercise the read path under contention
            (void)h.size();
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    producer.join();
    reader.join();

    ASSERT_EQ(cbCount.load(), kSegments);
    auto history = reg.getHistory();
    ASSERT_EQ(history.size(), 200u);  // Bounded

    reg.clearCallback(CallbackSlot::NativeView);
    reg.clearHistory();
}

// ============================================================
// TranscriptRegistry — clearHistory is thread-safe (TSan)
// ============================================================
TEST(registry_concurrent_push_clear) {
    auto& reg = TranscriptRegistry::instance();
    reg.clearHistory();

    std::atomic<bool> done{false};

    std::thread pusher([&] {
        for (int i = 0; i < 500; i++) {
            TranscriptSegment seg;
            seg.text = "x";
            reg.pushSegment(seg);
        }
        done.store(true, std::memory_order_release);
    });

    std::thread clearer([&] {
        while (!done.load(std::memory_order_acquire)) {
            reg.clearHistory();
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    pusher.join();
    clearer.join();
    reg.clearHistory();
    // No crash, no TSan error = pass
}

// ============================================================
// Main
// ============================================================
TEST_MAIN("Transcript Module Tests")
