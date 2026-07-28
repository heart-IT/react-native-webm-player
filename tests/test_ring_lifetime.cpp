// Proof test for audit finding P0-1: the ring's destructor does not protect a
// consumer sitting in read()'s fast path.
//
// WebMStreamBuffer::read() has two paths. readSlow() takes a ConsumerActiveGuard,
// which increments consumerActiveCount_; the destructor waits (up to
// cfg_.shutdownGraceMs) for that count to drain before freeing buffer_.
//
// The FAST path — taken whenever data is already available, which is the normal
// case for a live stream — takes no guard. It memcpy's straight out of
// buffer_.get(). A consumer there is invisible to the destructor's grace period,
// so `delete` frees the memory mid-copy.
//
// Both platforms hit this: the iOS demux thread and the Android ExoPlayer loader
// thread both call read().
//
// The consumer thread calls into the ring EXACTLY ONCE per round and never
// touches it again. So a report from AddressSanitizer can only come from inside
// that one in-flight read — not from the trivial "kept using a freed pointer"
// case, which would prove nothing about the grace period.
//
// Its own binary because ASan aborts the process on first error, masking
// anything sequenced after it.
//
//   pre-fix:  AddressSanitizer: heap-use-after-free, non-zero exit
//   post-fix: exits 0
//
// Stress, not deterministic: the delete has to land inside the memcpy. Rounds
// are repeated to hit the window reliably; run it a few times before trusting a
// clean result.

#include "WebMStreamBuffer.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "MediaLog.h"

using media::WebMStreamBuffer;

namespace {

constexpr size_t kCapacity = 8u << 20;
constexpr int kRounds = 60;

WebMStreamBuffer::Config testConfig() {
  WebMStreamBuffer::Config cfg;
  cfg.minCapacityBytes = 0;
  return cfg;
}

// Fill the ring so read() takes the unguarded fast path immediately.
void fillCompletely(WebMStreamBuffer& ring) {
  const size_t kChunk = 256 * 1024;
  std::vector<uint8_t> chunk(kChunk, 0x5A);
  size_t written = 0;
  while (written < kCapacity) {
    size_t got = ring.write(chunk.data(), std::min(kChunk, kCapacity - written), false);
    if (got == 0) break;
    written += got;
  }
}

}  // namespace

int main() {
  media::log::minLevel().store(media::log::Level::Error,
                               std::memory_order_relaxed);

  printf("\n=== Ring Lifetime Proof (P0-1) ===\n\n");
  printf("  %d rounds: fill 8MiB, one in-flight read, destroy mid-copy\n\n",
         kRounds);
  fflush(stdout);

  for (int round = 0; round < kRounds; round++) {
    auto* ring = new WebMStreamBuffer(1024, testConfig());
    fillCompletely(*ring);

    std::atomic<bool> aboutToRead{false};
    // Whole-capacity read => one long memcpy, widening the window the
    // destructor is supposed to cover.
    std::vector<uint8_t> dst(kCapacity);

    std::thread consumer([&] {
      aboutToRead.store(true, std::memory_order_release);
      // The only touch of `ring` on this thread, for its whole lifetime.
      (void)ring->read(dst.data(), dst.size(), 0);
    });

    while (!aboutToRead.load(std::memory_order_acquire)) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::microseconds(200));

    // Consumer is inside read(). The grace period should hold this off.
    delete ring;

    consumer.join();

    printf("  round %2d/%d survived\n", round + 1, kRounds);
    fflush(stdout);
  }

  printf("\n--- No use-after-free observed across %d rounds ---\n\n", kRounds);
  return 0;
}
