// Tests for the handle registry added for audit finding P0-3: an integer handle
// across the JNI boundary had no lifetime semantics, so a call issued after
// nativeDestroyRing dereferenced freed memory.
//
// Scope, stated precisely because it is narrower than it first appears:
//
//   * The contract these tests pin down is acquire-after-release returning null.
//     That is what makes a late call fail cleanly instead of dereferencing a
//     dangling pointer, and it is the part of P0-3 the registry actually fixes.
//
//   * The concurrent test below does NOT discriminate between this design and
//     the old raw-pointer one. Mutating acquire() to return a non-owning pointer
//     — exactly the old semantics — leaves all of these passing, because the
//     P0-1 fix already makes ~WebMStreamBuffer wait for in-flight consumers.
//     It is kept as a regression test for that combination, not as proof of P0-3.
//
//   * The JNI bridge itself cannot be exercised here (it needs a JVM). That the
//     bridge resolves handles through the registry rather than reinterpret_cast
//     is verified by inspection and by the Android build.

#include "RingRegistry.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "MediaLog.h"
#include "test_common.h"

using media::RingRegistry;
using media::WebMStreamBuffer;

namespace {

struct SilenceLogs {
  SilenceLogs() {
    media::log::minLevel().store(media::log::Level::Error,
                                 std::memory_order_relaxed);
  }
};
SilenceLogs g_silence_logs;

constexpr size_t kCapacity = 8u << 20;

WebMStreamBuffer::Config testConfig() {
  WebMStreamBuffer::Config cfg;
  cfg.minCapacityBytes = 0;
  return cfg;
}

int64_t makeRing() {
  return RingRegistry::instance().create(1024, testConfig());
}

void fillCompletely(WebMStreamBuffer& ring) {
  const size_t kChunk = 256 * 1024;
  std::vector<uint8_t> chunk(kChunk, 0x5A);
  size_t written = 0;
  while (written < kCapacity) {
    size_t got = ring.write(chunk.data(), kChunk, false);
    if (got == 0) break;
    written += got;
  }
}

}  // namespace

TEST(create_returns_a_resolvable_handle) {
  int64_t handle = makeRing();
  ASSERT_GT(handle, int64_t(0));
  auto ring = RingRegistry::instance().acquire(handle);
  ASSERT_TRUE(ring != nullptr);
  ASSERT_EQ(ring->capacity(), kCapacity);
  RingRegistry::instance().release(handle);
}

TEST(handles_are_distinct_across_instances) {
  int64_t a = makeRing();
  int64_t b = makeRing();
  ASSERT_NE(a, b);
  RingRegistry::instance().release(a);
  RingRegistry::instance().release(b);
}

// The core contract: a late call — the one ExoPlayer's loader can still issue
// after release, because release() can time out and the Loader's executor is
// not awaited — resolves to nothing instead of a dangling pointer.
TEST(acquire_after_release_returns_null_rather_than_a_dangling_pointer) {
  int64_t handle = makeRing();
  RingRegistry::instance().release(handle);
  ASSERT_TRUE(RingRegistry::instance().acquire(handle) == nullptr);
}

TEST(acquire_of_an_unknown_handle_returns_null) {
  ASSERT_TRUE(RingRegistry::instance().acquire(0) == nullptr);
  ASSERT_TRUE(RingRegistry::instance().acquire(999999) == nullptr);
}

TEST(releasing_twice_is_harmless) {
  int64_t handle = makeRing();
  RingRegistry::instance().release(handle);
  RingRegistry::instance().release(handle);
  ASSERT_TRUE(RingRegistry::instance().acquire(handle) == nullptr);
}

TEST(release_drops_the_registry_entry) {
  size_t before = RingRegistry::instance().size();
  int64_t handle = makeRing();
  ASSERT_EQ(RingRegistry::instance().size(), before + 1);
  RingRegistry::instance().release(handle);
  ASSERT_EQ(RingRegistry::instance().size(), before);
}

// Regression test for the P0-1 fix combined with the registry: a consumer
// resolves a handle and reads while another thread releases it. Passes under
// both ownership models (see the scope note at the top), so treat a green
// result as "the combination still holds", not as evidence about P0-3.
TEST(a_reader_holding_a_resolved_handle_survives_a_concurrent_release) {
  for (int round = 0; round < 25; round++) {
    int64_t handle = makeRing();
    {
      auto seed = RingRegistry::instance().acquire(handle);
      ASSERT_TRUE(seed != nullptr);
      fillCompletely(*seed);
    }

    std::atomic<bool> reading{false};
    std::atomic<int> result{0};

    std::thread consumer([&] {
      auto ring = RingRegistry::instance().acquire(handle);
      if (!ring) return;  // released before we resolved — legitimate
      std::vector<uint8_t> dst(kCapacity);
      reading.store(true, std::memory_order_release);
      result.store(ring->read(dst.data(), dst.size(), 0),
                   std::memory_order_release);
    });

    while (!reading.load(std::memory_order_acquire)) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::microseconds(200));

    RingRegistry::instance().release(handle);
    consumer.join();

    ASSERT_TRUE(RingRegistry::instance().acquire(handle) == nullptr);
  }
}

TEST_MAIN("Ring Registry Proof (P0-3)")
