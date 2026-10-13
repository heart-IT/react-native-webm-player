// Hostile-input demux tests: untrusted track metadata, a cluster that can
// never fit the parse window, and truncation mid-header —
// the real fixture arriving in trickles small enough that every element
// boundary lands mid-chunk, exercising each "need more data" branch. Run
// under all three sanitizer suites; the sampling-rate test exists
// specifically for UBSan, which aborts on the double→int cast the unfixed
// code performed.
//
// Build/run: part of the sanitizer suites (see tests/CMakeLists.txt).

#include "test_common.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "demux/WebmDemuxer.h"
#include "common/MediaLog.h"
#include "fixtures/webm_fixture.h"
#include "webm_synth.h"

namespace {
// The wedge tests flood the window; silence the (throttled) warnings.
struct SilenceLogs {
  SilenceLogs() {
    media::log::minLevel().store(media::log::Level::Error,
                                 std::memory_order_relaxed);
  }
};
SilenceLogs g_silence_logs;
}  // namespace

using namespace media::demux;

// ---- SamplingFrequency beyond int range must not be UB ----

TEST(huge_sampling_frequency_is_rejected_not_ub) {
    // The synthetic header stores 48000.0f at a known offset; raising the
    // exponent byte 0x47 → 0x7E yields ~4.3e37 — far beyond int range. The
    // unfixed static_cast<int> of that double is UB (UBSan aborts here).
    std::vector<uint8_t> stream(
        webm_synth::kMinimalWebmHeader,
        webm_synth::kMinimalWebmHeader + sizeof(webm_synth::kMinimalWebmHeader));
    ASSERT_EQ(stream[webm_synth::kSamplingRateExponentOffset], 0x47);
    stream[webm_synth::kSamplingRateExponentOffset] = 0x7E;

    WebmDemuxer demuxer;
    demuxer.feedData(stream.data(), stream.size());

    // The stream is otherwise valid: tracks parse, streaming begins, and the
    // hostile rate is surfaced as 0 — the value the decoder config downstream
    // rejects cleanly — instead of an arbitrary wrapped int.
    ASSERT_EQ(demuxer.parseState(), ParseState::Streaming);
    auto info = demuxer.trackInfo();
    ASSERT_EQ(info.audioTrackNum, 1);
    ASSERT_EQ(info.audioSampleRate, 0);
    ASSERT_EQ(info.audioChannels, 2);
}

TEST(huge_sampling_frequency_in_real_fixture_is_rejected_not_ub) {
    // Same repro built from the real muxer-produced fixture, as the audit did:
    // find its SamplingFrequency element (B5, 8-byte double 48000.0) and raise
    // the exponent byte (0x40 → 0x7E is ~1e146).
    std::vector<uint8_t> stream(fixture::kWebmData,
                                fixture::kWebmData + fixture::kWebmDataSize);
    const uint8_t pattern[] = {0xB5, 0x88, 0x40, 0xE7, 0x70, 0x00};
    auto it = std::search(stream.begin(), stream.end(),
                          pattern, pattern + sizeof(pattern));
    ASSERT_TRUE(it != stream.end());
    *(it + 2) = 0x7E;

    WebmDemuxer demuxer;
    size_t offset = 0;
    while (offset < stream.size()) {
        size_t chunk = std::min(size_t{4096}, stream.size() - offset);
        demuxer.feedData(stream.data() + offset, chunk);
        offset += chunk;
    }

    ASSERT_EQ(demuxer.parseState(), ParseState::Streaming);
    ASSERT_EQ(demuxer.trackInfo().audioSampleRate, 0);
}

TEST(valid_sampling_frequency_still_parses) {
    // Guard the guard: the clamp must not reject the mandated 48 kHz.
    WebmDemuxer demuxer;
    demuxer.feedData(webm_synth::kMinimalWebmHeader,
                     sizeof(webm_synth::kMinimalWebmHeader));
    ASSERT_EQ(demuxer.parseState(), ParseState::Streaming);
    ASSERT_EQ(demuxer.trackInfo().audioSampleRate, 48000);
}

// ---- A cluster larger than the parse window must error, not wedge ----

TEST(oversized_cluster_surfaces_permanent_error) {
    // Valid header + one good cluster + a cluster declaring a 3 MiB body.
    // Compaction cannot trim past the current cluster's start, so a cluster
    // larger than the 2 MiB window can never be fully resident. The unfixed
    // code waited forever: parseState stayed Streaming, no error surfaced,
    // and only the backpressure drop counters climbed over frozen A/V.
    std::vector<uint8_t> good(
        webm_synth::kMinimalWebmHeader,
        webm_synth::kMinimalWebmHeader + sizeof(webm_synth::kMinimalWebmHeader));
    good.insert(good.end(), webm_synth::kXiphLacedCluster,
                webm_synth::kXiphLacedCluster +
                    sizeof(webm_synth::kXiphLacedCluster));

    WebmDemuxer demuxer;
    demuxer.feedData(good.data(), good.size());
    ASSERT_EQ(demuxer.parseState(), ParseState::Streaming);

    // Now the oversized cluster arrives, followed by its body the way a live
    // feed would deliver it. The error must surface long before the declared
    // 3 MiB — and certainly must not leave the demuxer silently Streaming
    // after the window has filled (64 KiB × 40 = 2.5 MiB > the 2 MiB window).
    std::string firstError;
    auto record = [&](const DemuxResult& result) {
        if (!result.error.empty() && firstError.empty()) {
            firstError = result.error;
        }
    };
    record(demuxer.feedData(webm_synth::kOversizedClusterPrefix,
                            sizeof(webm_synth::kOversizedClusterPrefix)));
    std::vector<uint8_t> body(65536, 0x00);
    for (int i = 0; i < 40 && demuxer.parseState() != ParseState::Error; ++i) {
        record(demuxer.feedData(body.data(), body.size()));
    }

    ASSERT_EQ(demuxer.parseState(), ParseState::Error);
    // The caller-visible error names the cause (the pump logs result.error
    // and reports health "failed" off this state).
    ASSERT_TRUE(firstError.find("cluster exceeds parse window") != std::string::npos);
}

TEST(oversized_first_cluster_surfaces_permanent_error) {
    // Same attack without a preceding good cluster: the oversized cluster is
    // the first one parseBlocks ever adopts.
    std::vector<uint8_t> stream(
        webm_synth::kMinimalWebmHeader,
        webm_synth::kMinimalWebmHeader + sizeof(webm_synth::kMinimalWebmHeader));
    stream.insert(stream.end(), webm_synth::kOversizedClusterPrefix,
                  webm_synth::kOversizedClusterPrefix +
                      sizeof(webm_synth::kOversizedClusterPrefix));

    WebmDemuxer demuxer;
    demuxer.feedData(stream.data(), stream.size());

    std::vector<uint8_t> body(65536, 0x00);
    for (int i = 0; i < 40; ++i) {
        demuxer.feedData(body.data(), body.size());
        if (demuxer.parseState() == ParseState::Error) break;
    }
    ASSERT_EQ(demuxer.parseState(), ParseState::Error);
}

// ---- Truncation mid-header: the "need more data" branches ----
//
// Feeding the real fixture in tiny chunks guarantees every header element
// (EBML, Segment, Tracks) is seen truncated many times before it completes.
// The parser must treat each truncation as "wait for more bytes" — never as a
// parse error (three of those and the demuxer goes permanently to Error) —
// and the packets that eventually come out must be exactly the ones a
// single-shot feed produces.
//
// The two parity tests assert the behaviour the system is supposed to have,
// and fail against unfixed code (the audit-proof convention): as of writing,
// WebmBlockParser's readerDataAt-miss path advances past a cluster-final
// block whose payload has not arrived yet, losing that packet forever
// (observed: the fixture's last video frame, pts 967000, under both trickle
// sizes).

namespace {

struct TrickleResult {
    std::vector<int64_t> audioPts;
    std::vector<int64_t> videoPts;
    std::string firstError;
    bool errorBeforeStreaming = false;
    ParseState finalState = ParseState::WaitingForEBML;
};

TrickleResult feedFixtureInChunks(size_t chunkSize) {
    WebmDemuxer demuxer;
    TrickleResult out;
    size_t offset = 0;
    while (offset < fixture::kWebmDataSize) {
        size_t chunk = std::min(chunkSize, fixture::kWebmDataSize - offset);
        const auto& result = demuxer.feedData(fixture::kWebmData + offset, chunk);
        for (const auto& pkt : result.audioPackets) out.audioPts.push_back(pkt.ptsUs);
        for (const auto& pkt : result.videoPackets) out.videoPts.push_back(pkt.ptsUs);
        if (!result.error.empty() && out.firstError.empty()) {
            out.firstError = result.error;
            if (demuxer.parseState() != ParseState::Streaming) {
                out.errorBeforeStreaming = true;
            }
        }
        offset += chunk;
    }
    out.finalState = demuxer.parseState();
    return out;
}

}  // namespace

TEST(byte_at_a_time_reaches_streaming_without_error) {
    auto r = feedFixtureInChunks(1);
    ASSERT_FALSE(r.errorBeforeStreaming);
    ASSERT_EQ(r.firstError, std::string());
    ASSERT_EQ(r.finalState, ParseState::Streaming);
}

TEST(byte_at_a_time_emits_same_packets_as_single_shot) {
    auto whole = feedFixtureInChunks(fixture::kWebmDataSize);
    // The baseline itself must be a real parse — equal-and-empty proves nothing.
    ASSERT_EQ(whole.finalState, ParseState::Streaming);
    ASSERT_EQ(whole.firstError, std::string());
    ASSERT_GE(whole.audioPts.size(), 40u);   // ~50 Opus packets in the 1s fixture
    ASSERT_GE(whole.videoPts.size(), 25u);   // ~30 VP9 frames

    auto trickle = feedFixtureInChunks(1);
    ASSERT_EQ(trickle.audioPts.size(), whole.audioPts.size());
    ASSERT_EQ(trickle.videoPts.size(), whole.videoPts.size());
    ASSERT_TRUE(trickle.audioPts == whole.audioPts);
    ASSERT_TRUE(trickle.videoPts == whole.videoPts);
}

TEST(seven_byte_chunks_emit_same_packets_as_single_shot) {
    // 7 is coprime to every element size in the fixture, so chunk boundaries
    // drift through ID/size/payload positions instead of repeating one cut.
    auto whole = feedFixtureInChunks(fixture::kWebmDataSize);
    auto trickle = feedFixtureInChunks(7);
    ASSERT_FALSE(trickle.errorBeforeStreaming);
    ASSERT_EQ(trickle.firstError, std::string());
    ASSERT_EQ(trickle.finalState, ParseState::Streaming);
    ASSERT_EQ(trickle.audioPts.size(), whole.audioPts.size());
    ASSERT_EQ(trickle.videoPts.size(), whole.videoPts.size());
    ASSERT_TRUE(trickle.audioPts == whole.audioPts);
    ASSERT_TRUE(trickle.videoPts == whole.videoPts);
}

// ---- Feeds that end mid-cluster must keep the parse window trimmed ----
//
// Every feed of a live stream ends mid-cluster, so parseBlocks() leaves
// through a "need more data" exit on nearly every call. Those exits returned
// before the compaction schedule, so the window was never trimmed: on the
// iPhone 11 (2026-10-12) it filled at kMaxBufferSize after ~80 s and every
// later feed was dropped as "demuxer buffer overflow", with no rejoin. The
// bound is what deferred compaction allows: the cluster scheduled by the
// previous feed, the current one, and this feed's bytes. The unfixed code
// holds the whole stream.

namespace {

// One cluster holding a single 3000-byte SimpleBlock on track 1: a block the
// size of a real frame, so a chunk boundary lands inside its payload. The
// two-byte Timecode keeps hundreds of clusters monotonic (the block-level
// dedup skips a repeated PTS).
constexpr size_t kFramePayloadBytes = 3000;
constexpr size_t kFrameClusterBytes = 17 + kFramePayloadBytes;

std::vector<uint8_t> frameCluster(uint16_t timecodeMs) {
    std::vector<uint8_t> c = {
        0x1F, 0x43, 0xB6, 0x75, 0x4B, 0xC3,  // Cluster, body = 3011 bytes
        0xE7, 0x82, static_cast<uint8_t>(timecodeMs >> 8), static_cast<uint8_t>(timecodeMs),
        0xA3, 0x4B, 0xBC,                    // SimpleBlock, body = 3004 bytes
        0x81, 0x00, 0x00, 0x80,              // track 1, offset 0, keyframe
    };
    c.resize(kFrameClusterBytes, 0x5A);
    return c;
}

}  // namespace

TEST(mid_cluster_feeds_keep_the_window_trimmed) {
    constexpr size_t kClusters = 200;
    constexpr size_t kChunk = 1000;
    std::vector<uint8_t> stream(
        webm_synth::kMinimalWebmHeader,
        webm_synth::kMinimalWebmHeader + sizeof(webm_synth::kMinimalWebmHeader));
    for (size_t i = 0; i < kClusters; ++i) {
        auto c = frameCluster(static_cast<uint16_t>(i * 20));
        stream.insert(stream.end(), c.begin(), c.end());
    }

    WebmDemuxer demuxer;
    size_t maxWindow = 0;
    size_t packets = 0;
    for (size_t offset = 0; offset < stream.size(); offset += kChunk) {
        size_t chunk = std::min(kChunk, stream.size() - offset);
        packets += demuxer.feedData(stream.data() + offset, chunk).audioPackets.size();
        maxWindow = std::max(maxWindow, demuxer.bufferBytes());
    }
    ASSERT_EQ(demuxer.parseState(), ParseState::Streaming);
    ASSERT_EQ(packets, kClusters);
    // Compaction trims to Cluster::GetPosition(), which mkvparser measures
    // from the Segment payload, so the trim point sits the EBML header (36
    // bytes) and the Segment header (12) before the cluster.
    constexpr size_t kSegmentPayloadOffset = 48;
    ASSERT_LE(maxWindow, 2 * kFrameClusterBytes + kChunk + kSegmentPayloadOffset);
}

TEST_MAIN("Hostile-Input Demux Tests")
