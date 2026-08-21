// Proof tests for the ring destructor's consumer-drain contract.
//
// Phase 1 — audit finding P0-1: the destructor must not free buffer_ while a
// consumer sits in read()'s fast path.
//
// Phase 2 — the shutdownGraceMs contract-violation branch: when a consumer is
// still inside read() after the grace window, the destructor logs the
// violation and KEEPS WAITING. It must never trade the wait for a
// use-after-free, and it must return once the consumer exits.
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
#include <memory>
#include <thread>
#include <vector>

#include "MediaLog.h"

using media::WebMStreamBuffer;

namespace {

constexpr size_t kCapacity = 8u << 20;
constexpr int kRounds = 60;

WebMStreamBuffer::Config testConfig() {
  WebMStreamBuffer::Config cfg;
  return cfg;
}

// Fill the ring so read() takes the unguarded fast path immediately.
void fillToCapacity(WebMStreamBuffer& ring, size_t capacity) {
  const size_t kChunk = 256 * 1024;
  std::vector<uint8_t> chunk(kChunk, 0x5A);
  size_t written = 0;
  while (written < capacity) {
    size_t got = ring.write(chunk.data(), std::min(kChunk, capacity - written));
    if (got == 0) break;
    written += got;
  }
}

// ---- Phase 2: the shutdownGraceMs contract-violation branch ----
//
// shutdownGraceMs = 1 and a whole-capacity read of 256 MiB into a cold
// (never-touched) destination: the copy page-faults its way through the
// window, holding the consumer inside read() past the grace (~5 ms even on a
// plain build; 17–47 ms under ASan). The destructor's drain loop makes its
// first post-grace elapsed check ~1 ms in, so a round whose read overlaps
// the delete by >= 4 ms exercised the elapsed>=grace branch (warn, then keep
// waiting) unless that one sleep overshot past the whole overlap — hence
// several qualifying rounds are collected, not one. Timing-dependent, so
// rounds repeat until enough qualify.
//
//   contract violated (destructor frees at/after grace): ASan
//     heap-use-after-free, and deleteMs collapses to ~grace while the read
//     runs on — both checked.
//   contract honored: delete blocks until the read exits; exits 0.

constexpr size_t kGraceCapacity = 256u << 20;
constexpr uint64_t kGraceMs = 1;
constexpr double kProvenOverlapMs = 4.0;
constexpr int kMaxGraceRounds = 40;
constexpr int kWantedProvenRounds = 3;

double msSince(std::chrono::steady_clock::time_point from,
               std::chrono::steady_clock::time_point to) {
  return std::chrono::duration<double, std::milli>(to - from).count();
}

bool graceContractRounds() {
  WebMStreamBuffer::Config cfg = testConfig();
  cfg.shutdownGraceMs = kGraceMs;

  printf("  grace contract: %llums grace vs a %zuMiB in-flight read\n\n",
         static_cast<unsigned long long>(kGraceMs), kGraceCapacity >> 20);
  fflush(stdout);

  int proven = 0;
  for (int round = 0; round < kMaxGraceRounds && proven < kWantedProvenRounds;
       round++) {
    auto* ring = new WebMStreamBuffer(kGraceCapacity, cfg);
    fillToCapacity(*ring, kGraceCapacity);

    // Cold pages: first touch happens inside the copy, stretching the read.
    std::unique_ptr<uint8_t[]> dst(new uint8_t[kGraceCapacity]);

    std::atomic<bool> aboutToRead{false};
    int readResult = 0;
    std::chrono::steady_clock::time_point tReadEnd;

    std::thread consumer([&] {
      aboutToRead.store(true, std::memory_order_release);
      // The only touch of `ring` on this thread, for its whole lifetime.
      readResult = ring->read(dst.get(), kGraceCapacity, 0);
      tReadEnd = std::chrono::steady_clock::now();
    });

    while (!aboutToRead.load(std::memory_order_acquire)) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::microseconds(200));

    auto tDelStart = std::chrono::steady_clock::now();
    delete ring;
    auto tDelEnd = std::chrono::steady_clock::now();

    consumer.join();

    if (readResult != static_cast<int>(kGraceCapacity)) {
      printf("  FAIL: read returned %d, expected %zu — copy did not complete\n",
             readResult, kGraceCapacity);
      return false;
    }

    double overlapMs = msSince(tDelStart, tReadEnd);
    double deleteMs = msSince(tDelStart, tDelEnd);
    bool qualifies = overlapMs >= kProvenOverlapMs;
    if (qualifies) {
      proven++;
      // The read outlived the grace window, so a destructor that gave up at
      // the grace would have returned ~overlapMs early. 5 ms of slack covers
      // the scheduling gap between the guard release inside read() and the
      // consumer recording tReadEnd.
      if (deleteMs + 5.0 < overlapMs) {
        printf("  FAIL: destructor returned after %.1fms but the consumer was "
               "inside read() for %.1fms past delete\n", deleteMs, overlapMs);
        return false;
      }
    }
    printf("  round %2d: overlap=%6.1fms delete=%6.1fms %s\n", round + 1,
           overlapMs, deleteMs, qualifies ? "(past grace)" : "");
    fflush(stdout);
  }

  if (proven == 0) {
    printf("  FAIL: no round held the consumer past the %llums grace window — "
           "the branch was never exercised\n",
           static_cast<unsigned long long>(kGraceMs));
    return false;
  }
  printf("\n--- Grace-violation branch exercised in %d round(s); destructor "
         "waited out every read ---\n\n", proven);
  return true;
}

}  // namespace

int main() {
  media::log::minLevel().store(media::log::Level::Error,
                               std::memory_order_relaxed);

  printf("\n=== Ring Lifetime Proofs (P0-1 + grace contract) ===\n\n");
  printf("  %d rounds: fill 8MiB, one in-flight read, destroy mid-copy\n\n",
         kRounds);
  fflush(stdout);

  for (int round = 0; round < kRounds; round++) {
    auto* ring = new WebMStreamBuffer(1024, testConfig());
    fillToCapacity(*ring, kCapacity);

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

  return graceContractRounds() ? 0 : 1;
}
