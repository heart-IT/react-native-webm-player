// WebMStreamBuffer test suite.
//
// This is the one component both platforms sit on: the iOS demuxer and the
// Android ExoPlayer DataSource both read the bytes JS wrote through it. It
// shipped in the predecessor with zero test coverage, so every behaviour
// asserted here was read off the implementation rather than assumed.
//
// Run under each sanitizer:
//   cmake -B build-asan -S tests -DSANITIZER=address && cmake --build build-asan
//   ./build-asan/test_stream_buffer

#include "WebMStreamBuffer.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "MediaLog.h"
#include "test_common.h"

using media::WebMStreamBuffer;

namespace {

// The buffer logs on every overflow; a saturation test would flood stderr.
struct SilenceLogs {
  SilenceLogs() {
    media::log::minLevel().store(media::log::Level::Error,
                                 std::memory_order_relaxed);
  }
};
SilenceLogs g_silence_logs;

// MIN_CAPACITY inside the buffer is 8 MiB and the constructor rounds up to a
// power of two, so this is the smallest instance that can actually exist.
constexpr size_t kCapacity = 8u << 20;

WebMStreamBuffer::Config testConfig() {
  WebMStreamBuffer::Config cfg;
  cfg.minCapacityBytes = 0;  // let the hard 8 MiB floor decide
  return cfg;
}

// Ask for 1 KiB and get the floor.
WebMStreamBuffer makeBuffer() { return WebMStreamBuffer(1024, testConfig()); }

std::vector<uint8_t> pattern(size_t n, size_t startIndex = 0) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; i++)
    v[i] = static_cast<uint8_t>((startIndex + i) % 251);
  return v;
}

// isClusterBoundary=false: synthetic bytes are not real WebM clusters, and the
// debug-build validator would otherwise count and log a mismatch per write.
size_t put(WebMStreamBuffer& buf, const std::vector<uint8_t>& data) {
  return buf.write(data.data(), data.size(), false);
}

// Fill to exactly `bytes` using repeated writes.
size_t fill(WebMStreamBuffer& buf, size_t bytes) {
  const size_t kChunk = 64 * 1024;
  auto chunk = pattern(kChunk);
  size_t written = 0;
  while (written < bytes) {
    size_t want = std::min(kChunk, bytes - written);
    size_t got = buf.write(chunk.data(), want, false);
    if (got == 0) break;
    written += got;
  }
  return written;
}

}  // namespace

// ---------------------------------------------------------------- construction

TEST(capacity_is_floored_to_min_and_rounded_to_power_of_two) {
  auto buf = makeBuffer();
  ASSERT_EQ(buf.capacity(), kCapacity);
  ASSERT_EQ(buf.capacity() & (buf.capacity() - 1), size_t(0));
}

TEST(fresh_buffer_is_empty_and_not_terminal) {
  auto buf = makeBuffer();
  ASSERT_EQ(buf.sizeBytes(), uint64_t(0));
  ASSERT_FALSE(buf.isEndOfStream());
  ASSERT_FALSE(buf.isShutdown());
  ASSERT_FALSE(buf.isDestroyed());
}

// ----------------------------------------------------------------- write/read

TEST(write_then_read_roundtrips_bytes) {
  auto buf = makeBuffer();
  auto src = pattern(5);
  ASSERT_EQ(put(buf, src), size_t(5));

  uint8_t dst[16] = {};
  ASSERT_EQ(buf.read(dst, sizeof(dst), 50), 5);
  ASSERT_EQ(memcmp(dst, src.data(), 5), 0);
}

TEST(write_rejects_null_pointer_and_zero_length) {
  auto buf = makeBuffer();
  auto src = pattern(8);
  ASSERT_EQ(buf.write(nullptr, 8, false), size_t(0));
  ASSERT_EQ(buf.write(src.data(), 0, false), size_t(0));
  ASSERT_EQ(buf.sizeBytes(), uint64_t(0));
}

TEST(read_rejects_null_destination_and_zero_length) {
  auto buf = makeBuffer();
  put(buf, pattern(8));
  uint8_t dst[8] = {};
  ASSERT_EQ(buf.read(nullptr, sizeof(dst), 0), 0);
  ASSERT_EQ(buf.read(dst, 0, 0), 0);
}

TEST(size_bytes_tracks_written_minus_read) {
  auto buf = makeBuffer();
  put(buf, pattern(100));
  ASSERT_EQ(buf.sizeBytes(), uint64_t(100));

  uint8_t dst[40] = {};
  ASSERT_EQ(buf.read(dst, sizeof(dst), 50), 40);
  ASSERT_EQ(buf.sizeBytes(), uint64_t(60));
}

TEST(read_is_capped_by_max_len) {
  auto buf = makeBuffer();
  put(buf, pattern(1000));
  uint8_t dst[64] = {};
  ASSERT_EQ(buf.read(dst, sizeof(dst), 50), 64);
  ASSERT_EQ(buf.sizeBytes(), uint64_t(1000 - 64));
}

TEST(read_on_empty_buffer_returns_zero_after_timeout) {
  auto buf = makeBuffer();
  uint8_t dst[16] = {};
  auto start = std::chrono::steady_clock::now();
  ASSERT_EQ(buf.read(dst, sizeof(dst), 30), 0);
  auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();
  ASSERT_GE(elapsedMs, 25);
}

// ------------------------------------------------------------------ wraparound

TEST(ring_wraps_past_capacity_preserving_the_byte_stream) {
  auto buf = makeBuffer();
  // Deliberately NOT a divisor of the power-of-two capacity. With an aligned
  // chunk size (e.g. 64 KiB) every read offset stays aligned, the
  // copy-in-two-parts branch never executes, and a ring that ignored the
  // boundary entirely would still pass this test.
  const size_t kChunk = 65521;  // largest prime below 64 KiB
  // 24 MiB through an 8 MiB ring forces multiple wraps.
  const size_t kIterations = (24u << 20) / kChunk;

  std::vector<uint8_t> dst(kChunk);
  size_t streamIndex = 0;

  for (size_t it = 0; it < kIterations; it++) {
    auto src = pattern(kChunk, streamIndex);
    ASSERT_EQ(put(buf, src), kChunk);

    size_t drained = 0;
    while (drained < kChunk) {
      int n = buf.read(dst.data(), kChunk - drained, 50);
      ASSERT_GT(n, 0);
      for (int i = 0; i < n; i++) {
        ASSERT_EQ(dst[static_cast<size_t>(i)],
                  static_cast<uint8_t>((streamIndex + drained + i) % 251));
      }
      drained += static_cast<size_t>(n);
    }
    streamIndex += kChunk;
  }

  ASSERT_EQ(buf.sizeBytes(), uint64_t(0));
}

// -------------------------------------------------------------------- overflow

TEST(write_at_capacity_drops_the_whole_chunk_and_counts_it) {
  auto buf = makeBuffer();
  ASSERT_EQ(fill(buf, kCapacity), kCapacity);
  ASSERT_EQ(buf.sizeBytes(), uint64_t(kCapacity));

  auto extra = pattern(1024);
  ASSERT_EQ(put(buf, extra), size_t(0));  // full chunk refused

  auto stats = buf.getStats();
  ASSERT_GT(stats.bufferOverflows, uint64_t(0));
  ASSERT_GE(stats.droppedBytes, uint64_t(1024));
  ASSERT_GT(stats.consumerLagEvents, uint64_t(0));
}

// Writes are all-or-nothing. An earlier version of this test asserted the
// opposite — that a short write truncates the caller's chunk — which encoded a
// defect as a contract: the fragment left a truncated element in the byte stream
// for the demuxer to misparse. See tests/test_audit_proof.cpp.
TEST(write_with_partial_space_rejects_the_chunk_whole) {
  auto buf = makeBuffer();
  ASSERT_EQ(fill(buf, kCapacity - 10), kCapacity - 10);

  auto src = pattern(100);
  ASSERT_EQ(put(buf, src), size_t(0));  // 10 bytes free, 100 requested
  ASSERT_EQ(buf.sizeBytes(), uint64_t(kCapacity - 10));  // nothing accepted

  auto stats = buf.getStats();
  ASSERT_GE(stats.droppedBytes, uint64_t(100));  // the whole chunk is counted
  ASSERT_GT(stats.bufferOverflows, uint64_t(0));
}

TEST(is_consumer_lagging_trips_above_eighty_percent) {
  auto buf = makeBuffer();
  ASSERT_FALSE(buf.isConsumerLagging());
  fill(buf, static_cast<size_t>(kCapacity * 0.85));
  ASSERT_TRUE(buf.isConsumerLagging());
}

// --------------------------------------------------------- end of stream / shutdown

TEST(end_of_stream_makes_an_empty_read_return_minus_one) {
  auto buf = makeBuffer();
  buf.setEndOfStream(true);
  ASSERT_TRUE(buf.isEndOfStream());

  uint8_t dst[16] = {};
  ASSERT_EQ(buf.read(dst, sizeof(dst), 1000), -1);
}

TEST(end_of_stream_still_drains_buffered_bytes_first) {
  auto buf = makeBuffer();
  put(buf, pattern(32));
  buf.setEndOfStream(true);

  uint8_t dst[64] = {};
  ASSERT_EQ(buf.read(dst, sizeof(dst), 50), 32);
  ASSERT_EQ(buf.read(dst, sizeof(dst), 50), -1);
}

TEST(shutdown_makes_read_return_minus_one) {
  auto buf = makeBuffer();
  buf.shutdown();
  ASSERT_TRUE(buf.isShutdown());
  ASSERT_TRUE(buf.isEndOfStream());

  uint8_t dst[16] = {};
  ASSERT_EQ(buf.read(dst, sizeof(dst), 50), -1);
}

TEST(shutdown_drains_buffered_bytes_before_reporting_terminal) {
  auto buf = makeBuffer();
  put(buf, pattern(24));
  buf.shutdown();

  uint8_t dst[64] = {};
  ASSERT_EQ(buf.read(dst, sizeof(dst), 50), 24);
  ASSERT_EQ(buf.read(dst, sizeof(dst), 50), -1);
}

TEST(write_after_shutdown_is_refused) {
  auto buf = makeBuffer();
  buf.shutdown();
  ASSERT_EQ(put(buf, pattern(16)), size_t(0));
}

TEST(shutdown_is_idempotent) {
  auto buf = makeBuffer();
  buf.shutdown();
  buf.shutdown();
  ASSERT_TRUE(buf.isShutdown());
  ASSERT_TRUE(buf.getStats().shutdown);
}

TEST(shutdown_promptly_unblocks_a_blocked_reader) {
  auto buf = makeBuffer();
  std::atomic<int> result{-99};
  std::atomic<bool> entered{false};

  std::thread reader([&] {
    uint8_t dst[16] = {};
    entered.store(true, std::memory_order_release);
    result.store(buf.read(dst, sizeof(dst), 10000), std::memory_order_release);
  });

  while (!entered.load(std::memory_order_acquire))
    std::this_thread::yield();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto start = std::chrono::steady_clock::now();
  buf.shutdown();
  reader.join();
  auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();

  ASSERT_EQ(result.load(std::memory_order_acquire), -1);
  ASSERT_LT(elapsedMs, 5000);  // woken by the notify, not the 10s timeout
}

// ----------------------------------------------------------------------- clear

TEST(clear_drops_contents_and_resets_stats) {
  auto buf = makeBuffer();
  fill(buf, 256 * 1024);
  buf.setEndOfStream(true);

  buf.clear();

  ASSERT_EQ(buf.sizeBytes(), uint64_t(0));
  ASSERT_FALSE(buf.isEndOfStream());
  auto stats = buf.getStats();
  ASSERT_EQ(stats.totalBytesWritten, uint64_t(0));
  ASSERT_EQ(stats.totalBytesRead, uint64_t(0));
  ASSERT_EQ(stats.droppedBytes, uint64_t(0));
  ASSERT_EQ(stats.bufferOverflows, uint64_t(0));
}

TEST(clear_is_ignored_once_shutdown) {
  auto buf = makeBuffer();
  put(buf, pattern(128));
  buf.shutdown();

  buf.clear();

  ASSERT_EQ(buf.sizeBytes(), uint64_t(128));
  ASSERT_TRUE(buf.isShutdown());
}

// -------------------------------------------------------------- live / recovery

TEST(go_to_live_discards_the_entire_backlog) {
  auto buf = makeBuffer();
  fill(buf, 1024 * 1024);
  ASSERT_GT(buf.sizeBytes(), uint64_t(0));

  buf.goToLive();
  ASSERT_EQ(buf.sizeBytes(), uint64_t(0));
}

TEST(is_behind_live_compares_backlog_to_threshold) {
  auto buf = makeBuffer();
  put(buf, pattern(1000));
  ASSERT_TRUE(buf.isBehindLive(500));
  ASSERT_FALSE(buf.isBehindLive(2000));
}

TEST(soft_reset_trims_backlog_to_half_capacity) {
  auto buf = makeBuffer();
  const size_t sixMiB = 6u << 20;
  ASSERT_EQ(fill(buf, sixMiB), sixMiB);

  buf.softReset();

  ASSERT_EQ(buf.sizeBytes(), uint64_t(kCapacity / 2));
  ASSERT_EQ(buf.getRecoveryStats().softResets, uint64_t(1));
}

TEST(soft_reset_leaves_a_small_backlog_untouched) {
  auto buf = makeBuffer();
  put(buf, pattern(1000));

  buf.softReset();

  // Below half capacity there is nothing to trim, but the reset is still
  // recorded — the counter tracks invocations, not bytes reclaimed.
  ASSERT_EQ(buf.sizeBytes(), uint64_t(1000));
  ASSERT_EQ(buf.getRecoveryStats().softResets, uint64_t(1));
}

TEST(soft_reset_on_empty_buffer_is_a_no_op) {
  auto buf = makeBuffer();
  buf.softReset();
  ASSERT_EQ(buf.getRecoveryStats().softResets, uint64_t(0));
}

// ---------------------------------------------------------------------- health

TEST(health_is_healthy_when_fresh) {
  auto buf = makeBuffer();
  ASSERT_TRUE(buf.getHealthStatus() == WebMStreamBuffer::HealthStatus::Healthy);
}

TEST(health_is_dead_after_shutdown) {
  auto buf = makeBuffer();
  buf.shutdown();
  ASSERT_TRUE(buf.getHealthStatus() == WebMStreamBuffer::HealthStatus::Dead);
}

TEST(health_reports_severe_backpressure_when_nearly_full) {
  auto buf = makeBuffer();
  fill(buf, kCapacity);
  ASSERT_TRUE(buf.getHealthStatus() ==
              WebMStreamBuffer::HealthStatus::SevereBackpressure);
}

TEST(health_reports_producer_stall_after_the_configured_window) {
  WebMStreamBuffer::Config cfg = testConfig();
  cfg.producerStallMs = 20;
  WebMStreamBuffer buf(1024, cfg);

  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  ASSERT_TRUE(buf.getHealthStatus() ==
              WebMStreamBuffer::HealthStatus::ProducerStalled);
}

// ----------------------------------------------------------------------- stats

TEST(stats_report_capacity_and_terminal_flags) {
  auto buf = makeBuffer();
  auto stats = buf.getStats();
  ASSERT_EQ(stats.capacityBytes, kCapacity);
  ASSERT_FALSE(stats.endOfStream);
  ASSERT_FALSE(stats.shutdown);
}

TEST(stats_account_for_every_byte_written_and_read) {
  auto buf = makeBuffer();
  const size_t kTotal = 512 * 1024;
  ASSERT_EQ(fill(buf, kTotal), kTotal);

  std::vector<uint8_t> dst(64 * 1024);
  size_t drained = 0;
  while (drained < kTotal) {
    int n = buf.read(dst.data(), dst.size(), 50);
    ASSERT_GT(n, 0);
    drained += static_cast<size_t>(n);
  }

  // Producer/consumer tallies are flushed to the shared counters at most every
  // 50ms, so a getStats() immediately after the last read can still be behind.
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  auto stats = buf.getStats();
  ASSERT_EQ(stats.totalBytesWritten, uint64_t(kTotal));
  ASSERT_EQ(stats.totalBytesRead, uint64_t(kTotal));
  ASSERT_EQ(stats.droppedBytes, uint64_t(0));
}

// ------------------------------------------------------------------------ SPSC

TEST(concurrent_producer_and_consumer_preserve_the_byte_stream) {
  auto buf = makeBuffer();
  const size_t kChunk = 8191;      // unaligned, so reads straddle the boundary
  const size_t kTotal = 4u << 20;  // stays under capacity, so nothing drops

  std::atomic<bool> mismatch{false};
  std::atomic<size_t> consumed{0};

  std::thread consumer([&] {
    std::vector<uint8_t> dst(kChunk);
    size_t index = 0;
    while (index < kTotal) {
      // Cap the last read so the consumed total lands exactly on kTotal; the
      // producer overshoots slightly because kTotal is not a multiple of kChunk.
      size_t want = std::min(dst.size(), kTotal - index);
      int n = buf.read(dst.data(), want, 100);
      if (n < 0) break;
      if (n == 0) continue;
      for (int i = 0; i < n; i++) {
        if (dst[static_cast<size_t>(i)] !=
            static_cast<uint8_t>((index + i) % 251)) {
          mismatch.store(true, std::memory_order_relaxed);
          return;
        }
      }
      index += static_cast<size_t>(n);
      consumed.store(index, std::memory_order_relaxed);
    }
  });

  std::thread producer([&] {
    size_t index = 0;
    while (index < kTotal) {
      auto src = pattern(kChunk, index);
      size_t off = 0;
      while (off < kChunk) {
        size_t n = buf.write(src.data() + off, kChunk - off, false);
        if (n == 0) {
          std::this_thread::yield();
          continue;
        }
        off += n;
      }
      index += kChunk;
    }
    buf.setEndOfStream(true);
  });

  producer.join();
  consumer.join();

  ASSERT_FALSE(mismatch.load(std::memory_order_relaxed));
  ASSERT_EQ(consumed.load(std::memory_order_relaxed), kTotal);
}

TEST_MAIN("WebMStreamBuffer Tests")
