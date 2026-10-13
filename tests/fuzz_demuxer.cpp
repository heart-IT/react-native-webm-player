// Fuzz test for WebmDemuxer — the untrusted input boundary.
//
// Two modes:
//   1. libFuzzer (clang -fsanitize=fuzzer): automated coverage-guided fuzzing
//   2. Standalone (no libFuzzer): corpus replay + built-in mutation harness
//
// Build:
//   cmake -B tests/sanitizer/build-fuzz -S tests/sanitizer -DSANITIZER=address -DFUZZ=ON
//   cmake --build tests/sanitizer/build-fuzz
//
// Run (libFuzzer):
//   tests/sanitizer/build-fuzz/fuzz_demuxer tests/sanitizer/corpus/
//
// Run (standalone):
//   tests/sanitizer/build-fuzz/fuzz_demuxer

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <random>

#include "demux/WebmDemuxer.h"
#include "webm_synth.h"

// The seed header lives in webm_synth.h, shared with the deterministic demux
// tests so its sizes are exercised from both directions.
static const uint8_t* const kMinimalWebmHeader = webm_synth::kMinimalWebmHeader;
static constexpr size_t kMinimalWebmHeaderSize = sizeof(webm_synth::kMinimalWebmHeader);

// Startup self-check: the pristine seed must genuinely reach Streaming, or
// every header-based phase silently degrades to fuzzing the first parser
// stage only: a seed with inconsistent element sizes once hid the
// SamplingFrequency int-cast UB from 100k CI iterations.
static void verifySeedReachesStreaming() {
    media::demux::WebmDemuxer demuxer;
    demuxer.feedData(kMinimalWebmHeader, kMinimalWebmHeaderSize);
    if (demuxer.parseState() != media::demux::ParseState::Streaming) {
        fprintf(stderr,
                "FATAL: fuzz seed no longer reaches ParseState::Streaming "
                "(state=%d) — fix the element sizes in tests/webm_synth.h\n",
                static_cast<int>(demuxer.parseState()));
        abort();
    }
}

// Exercises the demuxer with a single contiguous input.
// Returns false if ASAN/UBSAN would have already aborted on a real bug.
static void fuzzOnce(const uint8_t* data, size_t size) {
    media::demux::WebmDemuxer demuxer;

    // Strategy 1: Feed all at once
    const auto& result = demuxer.feedData(data, size);

    // Touch all returned data to ensure pointers are valid
    for (const auto& pkt : result.audioPackets) {
        if (pkt.data && pkt.size > 0) {
            volatile uint8_t sink = pkt.data[0];
            volatile uint8_t sink2 = pkt.data[pkt.size - 1];
            (void)sink; (void)sink2;
        }
    }
    for (const auto& pkt : result.videoPackets) {
        if (pkt.data && pkt.size > 0) {
            volatile uint8_t sink = pkt.data[0];
            volatile uint8_t sink2 = pkt.data[pkt.size - 1];
            (void)sink; (void)sink2;
        }
    }

    (void)demuxer.bufferBytes();
    (void)demuxer.trackInfo();
    (void)demuxer.parseState();
}

// Exercises incremental feeding — splits input into random-sized chunks.
static void fuzzIncremental(const uint8_t* data, size_t size, std::mt19937& rng) {
    media::demux::WebmDemuxer demuxer;

    size_t offset = 0;
    while (offset < size) {
        // Random chunk size: 1 to 4096 bytes
        size_t chunk = 1 + (rng() % std::min(size_t{4096}, size - offset));
        chunk = std::min(chunk, size - offset);

        const auto& result = demuxer.feedData(data + offset, chunk);

        // Touch all packet data pointers
        for (const auto& pkt : result.audioPackets) {
            if (pkt.data && pkt.size > 0) {
                volatile uint8_t sink = pkt.data[0];
                (void)sink;
            }
        }
        for (const auto& pkt : result.videoPackets) {
            if (pkt.data && pkt.size > 0) {
                volatile uint8_t sink = pkt.data[0];
                (void)sink;
            }
        }

        offset += chunk;
    }

    // Test reset + re-feed
    demuxer.reset();
    if (size > 0) {
        demuxer.feedData(data, std::min(size, size_t{1024}));
    }
}

// Exercises the reset path during various parse states.
static void fuzzResetDuring(const uint8_t* data, size_t size, std::mt19937& rng) {
    media::demux::WebmDemuxer demuxer;

    size_t offset = 0;
    int feedCount = 0;
    int resetAt = 1 + static_cast<int>(rng() % 5);

    while (offset < size) {
        size_t chunk = 1 + (rng() % std::min(size_t{2048}, size - offset));
        chunk = std::min(chunk, size - offset);

        demuxer.feedData(data + offset, chunk);
        offset += chunk;
        feedCount++;

        if (feedCount == resetAt) {
            demuxer.reset();
            // Re-feed from start
            if (size > 0) {
                size_t refeed = std::min(size, size_t{512});
                demuxer.feedData(data, refeed);
            }
        }
    }
}

// --- libFuzzer entry point ---
extern "C" int LLVMFuzzerInitialize(int*, char***) {
    verifySeedReachesStreaming();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Cap input size to prevent OOM (demuxer caps at 2MB internally)
    if (size > 2 * 1024 * 1024) return 0;

    // Deterministic seed from input for reproducibility
    uint32_t seed = 0;
    if (size >= 4) std::memcpy(&seed, data, 4);
    std::mt19937 rng(seed);

    fuzzOnce(data, size);
    fuzzIncremental(data, size, rng);
    fuzzResetDuring(data, size, rng);

    return 0;
}

#ifndef FUZZING_BUILD_MODE_INSTRUMENTATION
// --- Standalone mode: run without libFuzzer ---

// Simple byte-level mutation
static void mutate(std::vector<uint8_t>& data, std::mt19937& rng) {
    if (data.empty()) return;

    int action = rng() % 6;
    switch (action) {
    case 0: // Flip random bit
        data[rng() % data.size()] ^= (1 << (rng() % 8));
        break;
    case 1: // Set random byte to interesting value
    {
        static const uint8_t interesting[] = {0, 1, 0x7F, 0x80, 0xFF, 0xFE, 0x00};
        data[rng() % data.size()] = interesting[rng() % sizeof(interesting)];
        break;
    }
    case 2: // Insert random byte
        if (data.size() < 65536) {
            size_t pos = rng() % (data.size() + 1);
            data.insert(data.begin() + static_cast<ptrdiff_t>(pos),
                        static_cast<uint8_t>(rng() & 0xFF));
        }
        break;
    case 3: // Delete random byte
        if (data.size() > 1) {
            data.erase(data.begin() + static_cast<ptrdiff_t>(rng() % data.size()));
        }
        break;
    case 4: // Overwrite chunk with zeros
    {
        size_t pos = rng() % data.size();
        size_t len = 1 + rng() % std::min(size_t{16}, data.size() - pos);
        std::memset(data.data() + pos, 0, len);
        break;
    }
    case 5: // Copy chunk within input
        if (data.size() > 4) {
            size_t src = rng() % (data.size() - 1);
            size_t dst = rng() % (data.size() - 1);
            size_t len = 1 + rng() % std::min(size_t{8}, data.size() - std::max(src, dst));
            std::memmove(data.data() + dst, data.data() + src, len);
        }
        break;
    }
}

int main(int argc, char** argv) {
    constexpr int kIterations = 100000;

    verifySeedReachesStreaming();

    printf("WebmDemuxer fuzz test (standalone mode, %d iterations)\n\n", kIterations);

    std::mt19937 rng(42);  // deterministic for CI

    // Phase 1: Fuzz with completely random data
    printf("  Phase 1: random bytes...\n");
    for (int i = 0; i < kIterations / 4; ++i) {
        size_t len = rng() % 4096;
        std::vector<uint8_t> data(len);
        for (auto& b : data) b = static_cast<uint8_t>(rng() & 0xFF);
        LLVMFuzzerTestOneInput(data.data(), data.size());
    }
    printf("    %d iterations OK\n", kIterations / 4);

    // Phase 2: Fuzz by mutating a valid WebM header
    printf("  Phase 2: mutated valid header...\n");
    for (int i = 0; i < kIterations / 4; ++i) {
        std::vector<uint8_t> data(kMinimalWebmHeader,
                                   kMinimalWebmHeader + kMinimalWebmHeaderSize);
        int mutations = 1 + static_cast<int>(rng() % 5);
        for (int m = 0; m < mutations; ++m) mutate(data, rng);
        LLVMFuzzerTestOneInput(data.data(), data.size());
    }
    printf("    %d iterations OK\n", kIterations / 4);

    // Phase 3: Fuzz with valid header + random cluster data appended
    printf("  Phase 3: valid header + random cluster data...\n");
    for (int i = 0; i < kIterations / 4; ++i) {
        std::vector<uint8_t> data(kMinimalWebmHeader,
                                   kMinimalWebmHeader + kMinimalWebmHeaderSize);
        // Append random "cluster" data
        size_t extraLen = rng() % 8192;
        for (size_t j = 0; j < extraLen; ++j)
            data.push_back(static_cast<uint8_t>(rng() & 0xFF));
        // Sometimes mutate the header portion too
        if (rng() % 3 == 0) mutate(data, rng);
        LLVMFuzzerTestOneInput(data.data(), data.size());
    }
    printf("    %d iterations OK\n", kIterations / 4);

    // Phase 4: Edge cases
    printf("  Phase 4: edge cases...\n");
    int edgeCases = 0;

    // Empty input
    LLVMFuzzerTestOneInput(nullptr, 0);
    edgeCases++;

    // Single byte (all values)
    for (int b = 0; b < 256; ++b) {
        uint8_t byte = static_cast<uint8_t>(b);
        LLVMFuzzerTestOneInput(&byte, 1);
        edgeCases++;
    }

    // EBML ID bytes only (truncated)
    for (size_t len = 1; len <= kMinimalWebmHeaderSize && len <= 64; ++len) {
        LLVMFuzzerTestOneInput(kMinimalWebmHeader, len);
        edgeCases++;
    }

    // Max-size fields (0xFF repeated)
    {
        std::vector<uint8_t> maxBytes(8192, 0xFF);
        LLVMFuzzerTestOneInput(maxBytes.data(), maxBytes.size());
        edgeCases++;
    }

    // Valid header, then 2MB of zeros (exercises buffer overflow path)
    {
        std::vector<uint8_t> data(kMinimalWebmHeader,
                                   kMinimalWebmHeader + kMinimalWebmHeaderSize);
        data.resize(data.size() + 2 * 1024 * 1024, 0);
        LLVMFuzzerTestOneInput(data.data(), data.size());
        edgeCases++;
    }

    // Remaining random iterations
    int remaining = kIterations / 4 - edgeCases;
    for (int i = 0; i < remaining; ++i) {
        // Alternate between header-based and random mutations
        std::vector<uint8_t> data(kMinimalWebmHeader,
                                   kMinimalWebmHeader + kMinimalWebmHeaderSize);
        int mutations = 1 + static_cast<int>(rng() % 10);
        for (int m = 0; m < mutations; ++m) mutate(data, rng);
        LLVMFuzzerTestOneInput(data.data(), data.size());
    }
    printf("    %d iterations OK\n", kIterations / 4);

    // Phase 5: header + Xiph-laced clusters, mutated — fuzzes the lacing and
    // block-iteration paths, which none of the earlier phases could reach.
    printf("  Phase 5: mutated laced-cluster stream...\n");
    std::vector<uint8_t> lacedSeed(kMinimalWebmHeader,
                                   kMinimalWebmHeader + kMinimalWebmHeaderSize);
    lacedSeed.insert(lacedSeed.end(), webm_synth::kXiphLacedCluster,
                     webm_synth::kXiphLacedCluster + sizeof(webm_synth::kXiphLacedCluster));
    lacedSeed.insert(lacedSeed.end(), webm_synth::kXiphLacedCluster2,
                     webm_synth::kXiphLacedCluster2 + sizeof(webm_synth::kXiphLacedCluster2));
    LLVMFuzzerTestOneInput(lacedSeed.data(), lacedSeed.size());  // pristine first
    for (int i = 0; i < kIterations / 4; ++i) {
        std::vector<uint8_t> data = lacedSeed;
        int mutations = 1 + static_cast<int>(rng() % 5);
        for (int m = 0; m < mutations; ++m) mutate(data, rng);
        LLVMFuzzerTestOneInput(data.data(), data.size());
    }
    printf("    %d iterations OK\n", kIterations / 4);

    printf("\n  ALL PHASES PASSED (%d total iterations)\n", kIterations + kIterations / 4);
    return 0;
}
#endif
