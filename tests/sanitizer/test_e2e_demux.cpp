// End-to-end test: feeds a real WebM stream (VP9 + Opus) through the full
// demux pipeline and validates packet correctness.
//
// Build: cmake -B tests/sanitizer/build-asan -S tests/sanitizer -DSANITIZER=address
//        cmake --build tests/sanitizer/build-asan --target test_e2e_demux
// Run:   tests/sanitizer/build-asan/test_e2e_demux

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>

#include "demux/WebmDemuxer.h"
#include "video/VP9HeaderParser.h"
#include "OpusDecoderAdapter.h"
#include "webm_fixture.h"

using namespace media;
using namespace media::demux;

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

#define ASSERT_GE(a, b) do { \
    auto _a = (a); auto _b = (b); \
    if (_a < _b) { \
        printf("FAIL\n    ASSERT_GE(%s, %s) [%lld < %lld] at %s:%d\n", \
               #a, #b, (long long)_a, (long long)_b, __FILE__, __LINE__); \
        g_tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_GT(a, b) do { \
    auto _a = (a); auto _b = (b); \
    if (_a <= _b) { \
        printf("FAIL\n    ASSERT_GT(%s, %s) [%lld <= %lld] at %s:%d\n", \
               #a, #b, (long long)_a, (long long)_b, __FILE__, __LINE__); \
        g_tests_failed++; \
        return; \
    } \
} while(0)

// Helper: feed entire fixture and collect all packets
struct CollectedPackets {
    std::vector<std::vector<uint8_t>> audioData;
    std::vector<int64_t> audioPts;
    std::vector<int64_t> audioDuration;
    std::vector<std::vector<uint8_t>> videoData;
    std::vector<int64_t> videoPts;
    std::vector<bool> videoKeyFrame;
    int clusterCount = 0;
    bool hadError = false;
    std::string lastError;
};

static CollectedPackets feedAll(WebmDemuxer& demuxer,
                                const uint8_t* data, size_t size, size_t chunkSize) {
    CollectedPackets out;
    size_t offset = 0;
    while (offset < size) {
        size_t chunk = std::min(chunkSize, size - offset);
        const auto& result = demuxer.feedData(data + offset, chunk);

        // Copy packet data (pointers only valid until next feedData)
        for (const auto& pkt : result.audioPackets) {
            out.audioData.emplace_back(pkt.data, pkt.data + pkt.size);
            out.audioPts.push_back(pkt.ptsUs);
            out.audioDuration.push_back(pkt.durationUs);
        }
        for (const auto& pkt : result.videoPackets) {
            out.videoData.emplace_back(pkt.data, pkt.data + pkt.size);
            out.videoPts.push_back(pkt.ptsUs);
            out.videoKeyFrame.push_back(pkt.isKeyFrame);
        }
        out.clusterCount += result.newClusterCount;
        if (!result.error.empty()) {
            out.hadError = true;
            out.lastError = result.error;
        }
        offset += chunk;
    }
    return out;
}

// ---- Tests ----

TEST(fixture_parses_without_error) {
    WebmDemuxer demuxer;
    auto pkts = feedAll(demuxer, fixture::kWebmData, fixture::kWebmDataSize, 4096);
    ASSERT_TRUE(!pkts.hadError);
    ASSERT_EQ(demuxer.parseState(), ParseState::Streaming);
}

TEST(track_info_correct) {
    WebmDemuxer demuxer;
    feedAll(demuxer, fixture::kWebmData, fixture::kWebmDataSize, 65536);

    const auto& info = demuxer.trackInfo();
    ASSERT_EQ(info.audioCodecId, std::string("A_OPUS"));
    ASSERT_EQ(info.videoCodecId, std::string("V_VP9"));
    ASSERT_EQ(info.videoWidth, 320);
    ASSERT_EQ(info.videoHeight, 240);
    ASSERT_TRUE(info.audioTrackNum >= 0);
    ASSERT_TRUE(info.videoTrackNum >= 0);
}

TEST(produces_audio_packets) {
    WebmDemuxer demuxer;
    auto pkts = feedAll(demuxer, fixture::kWebmData, fixture::kWebmDataSize, 4096);

    // 1 second of Opus @ 20ms frames = ~50 packets
    ASSERT_GE(pkts.audioData.size(), 40u);
    ASSERT_GE(pkts.audioPts.size(), 40u);
}

TEST(produces_video_packets) {
    WebmDemuxer demuxer;
    auto pkts = feedAll(demuxer, fixture::kWebmData, fixture::kWebmDataSize, 4096);

    // 1 second @ 30fps = ~30 frames
    ASSERT_GE(pkts.videoData.size(), 25u);
    ASSERT_GE(pkts.videoPts.size(), 25u);
}

TEST(audio_packets_have_valid_opus_data) {
    WebmDemuxer demuxer;
    auto pkts = feedAll(demuxer, fixture::kWebmData, fixture::kWebmDataSize, 65536);

    OpusDecoderAdapter decoder;
    ASSERT_TRUE(decoder.initialize(48000, 2));

    int successCount = 0;
    std::vector<float> pcm(960 * 2);  // Max Opus frame: 960 samples * 2 channels
    for (const auto& data : pkts.audioData) {
        int frames = decoder.decode(data.data(), data.size(), pcm.data(), 960);
        if (frames > 0) ++successCount;
    }

    // All packets should decode successfully
    ASSERT_EQ(static_cast<size_t>(successCount), pkts.audioData.size());
}

TEST(video_keyframe_has_valid_vp9_header) {
    WebmDemuxer demuxer;
    auto pkts = feedAll(demuxer, fixture::kWebmData, fixture::kWebmDataSize, 65536);

    // Find first keyframe
    bool foundKeyFrame = false;
    for (size_t i = 0; i < pkts.videoData.size(); ++i) {
        if (pkts.videoKeyFrame[i]) {
            auto info = vp9::parseHeader(pkts.videoData[i].data(), pkts.videoData[i].size());
            ASSERT_TRUE(info.valid);
            ASSERT_TRUE(info.isKeyFrame);
            ASSERT_EQ(info.width, 320);
            ASSERT_EQ(info.height, 240);
            foundKeyFrame = true;
            break;
        }
    }
    ASSERT_TRUE(foundKeyFrame);
}

TEST(all_video_frames_have_valid_vp9_marker) {
    WebmDemuxer demuxer;
    auto pkts = feedAll(demuxer, fixture::kWebmData, fixture::kWebmDataSize, 65536);

    for (size_t i = 0; i < pkts.videoData.size(); ++i) {
        auto info = vp9::parseHeader(pkts.videoData[i].data(), pkts.videoData[i].size());
        ASSERT_TRUE(info.valid);
    }
}

TEST(audio_pts_monotonically_increasing) {
    WebmDemuxer demuxer;
    auto pkts = feedAll(demuxer, fixture::kWebmData, fixture::kWebmDataSize, 4096);
    ASSERT_GE(pkts.audioPts.size(), 2u);

    for (size_t i = 1; i < pkts.audioPts.size(); ++i) {
        ASSERT_GE(pkts.audioPts[i], pkts.audioPts[i - 1]);
    }
}

TEST(video_pts_monotonically_increasing) {
    WebmDemuxer demuxer;
    auto pkts = feedAll(demuxer, fixture::kWebmData, fixture::kWebmDataSize, 4096);
    ASSERT_GE(pkts.videoPts.size(), 2u);

    for (size_t i = 1; i < pkts.videoPts.size(); ++i) {
        ASSERT_GE(pkts.videoPts[i], pkts.videoPts[i - 1]);
    }
}

TEST(audio_pts_spans_approximately_one_second) {
    WebmDemuxer demuxer;
    auto pkts = feedAll(demuxer, fixture::kWebmData, fixture::kWebmDataSize, 65536);
    ASSERT_GE(pkts.audioPts.size(), 2u);

    int64_t span = pkts.audioPts.back() - pkts.audioPts.front();
    // 1 second = 1,000,000 us. Allow 800ms-1200ms range.
    ASSERT_GE(span, 800000);
    ASSERT_GE(1200000, span);
}

TEST(video_pts_spans_approximately_one_second) {
    WebmDemuxer demuxer;
    auto pkts = feedAll(demuxer, fixture::kWebmData, fixture::kWebmDataSize, 65536);
    ASSERT_GE(pkts.videoPts.size(), 2u);

    int64_t span = pkts.videoPts.back() - pkts.videoPts.front();
    ASSERT_GE(span, 800000);
    ASSERT_GE(1200000, span);
}

TEST(first_video_frame_is_keyframe) {
    WebmDemuxer demuxer;
    auto pkts = feedAll(demuxer, fixture::kWebmData, fixture::kWebmDataSize, 65536);
    ASSERT_GE(pkts.videoKeyFrame.size(), 1u);
    ASSERT_TRUE(pkts.videoKeyFrame[0]);
}

TEST(audio_packet_sizes_reasonable) {
    WebmDemuxer demuxer;
    auto pkts = feedAll(demuxer, fixture::kWebmData, fixture::kWebmDataSize, 65536);

    for (const auto& data : pkts.audioData) {
        // Opus packets are typically 10-200 bytes. Reject empty or absurd sizes.
        ASSERT_GT(data.size(), 0u);
        ASSERT_GE(2000u, data.size());
    }
}

TEST(video_packet_sizes_reasonable) {
    WebmDemuxer demuxer;
    auto pkts = feedAll(demuxer, fixture::kWebmData, fixture::kWebmDataSize, 65536);

    for (const auto& data : pkts.videoData) {
        ASSERT_GT(data.size(), 0u);
        // VP9 frames at 320x240 should be well under 100KB
        ASSERT_GE(100000u, data.size());
    }
}

TEST(stream_header_captured) {
    WebmDemuxer demuxer;
    feedAll(demuxer, fixture::kWebmData, fixture::kWebmDataSize, 65536);

    ASSERT_TRUE(demuxer.hasStreamHeader());
    const auto& header = demuxer.streamHeader();
    // Header should contain EBML + Segment + Tracks (typically 100-500 bytes)
    ASSERT_GE(header.size(), 50u);
    ASSERT_GE(2000u, header.size());
    // Must start with EBML magic
    ASSERT_EQ(header[0], 0x1A);
    ASSERT_EQ(header[1], 0x45);
    ASSERT_EQ(header[2], 0xDF);
    ASSERT_EQ(header[3], 0xA3);
}

TEST(incremental_feeding_produces_same_packets) {
    // Feed in two different chunk sizes (both large enough to complete all clusters)
    WebmDemuxer demuxer1;
    auto pkts1 = feedAll(demuxer1, fixture::kWebmData, fixture::kWebmDataSize, 8192);

    WebmDemuxer demuxer2;
    auto pkts2 = feedAll(demuxer2, fixture::kWebmData, fixture::kWebmDataSize, 4096);

    // Same number of packets
    ASSERT_EQ(pkts1.audioData.size(), pkts2.audioData.size());
    ASSERT_EQ(pkts1.videoData.size(), pkts2.videoData.size());

    // Same PTS values
    for (size_t i = 0; i < pkts1.audioPts.size(); ++i) {
        ASSERT_EQ(pkts1.audioPts[i], pkts2.audioPts[i]);
    }
    for (size_t i = 0; i < pkts1.videoPts.size(); ++i) {
        ASSERT_EQ(pkts1.videoPts[i], pkts2.videoPts[i]);
    }

    // Same packet data
    for (size_t i = 0; i < pkts1.audioData.size(); ++i) {
        ASSERT_EQ(pkts1.audioData[i].size(), pkts2.audioData[i].size());
        ASSERT_TRUE(pkts1.audioData[i] == pkts2.audioData[i]);
    }
    for (size_t i = 0; i < pkts1.videoData.size(); ++i) {
        ASSERT_EQ(pkts1.videoData[i].size(), pkts2.videoData[i].size());
        ASSERT_TRUE(pkts1.videoData[i] == pkts2.videoData[i]);
    }
}

TEST(reset_and_refeed_produces_same_packets) {
    WebmDemuxer demuxer;
    auto pkts1 = feedAll(demuxer, fixture::kWebmData, fixture::kWebmDataSize, 4096);

    demuxer.reset();
    ASSERT_EQ(demuxer.parseState(), ParseState::WaitingForEBML);

    auto pkts2 = feedAll(demuxer, fixture::kWebmData, fixture::kWebmDataSize, 4096);

    ASSERT_EQ(pkts1.audioData.size(), pkts2.audioData.size());
    ASSERT_EQ(pkts1.videoData.size(), pkts2.videoData.size());

    for (size_t i = 0; i < pkts1.audioPts.size(); ++i) {
        ASSERT_EQ(pkts1.audioPts[i], pkts2.audioPts[i]);
    }
    for (size_t i = 0; i < pkts1.videoPts.size(); ++i) {
        ASSERT_EQ(pkts1.videoPts[i], pkts2.videoPts[i]);
    }
}

TEST(metrics_reflect_actual_output) {
    WebmDemuxer demuxer;
    auto pkts = feedAll(demuxer, fixture::kWebmData, fixture::kWebmDataSize, 4096);

    auto metrics = demuxer.demuxerMetrics();
    ASSERT_EQ(metrics.totalBytesFed, fixture::kWebmDataSize);
    ASSERT_EQ(metrics.audioPacketsEmitted, pkts.audioData.size());
    ASSERT_EQ(metrics.videoPacketsEmitted, pkts.videoData.size());
    ASSERT_EQ(metrics.overflowCount, 0u);
    ASSERT_EQ(metrics.blockStallCount, 0u);
    ASSERT_GT(metrics.feedDataCalls, 0u);
    ASSERT_EQ(metrics.parseState, ParseState::Streaming);
}

TEST(demuxer_reaches_streaming_state) {
    WebmDemuxer demuxer;
    feedAll(demuxer, fixture::kWebmData, fixture::kWebmDataSize, 4096);
    ASSERT_EQ(demuxer.parseState(), ParseState::Streaming);
}

TEST(audio_durations_are_20ms) {
    WebmDemuxer demuxer;
    auto pkts = feedAll(demuxer, fixture::kWebmData, fixture::kWebmDataSize, 65536);

    for (auto dur : pkts.audioDuration) {
        ASSERT_EQ(dur, 20000);  // 20ms in microseconds
    }
}

TEST(decoded_audio_contains_nonzero_samples) {
    WebmDemuxer demuxer;
    auto pkts = feedAll(demuxer, fixture::kWebmData, fixture::kWebmDataSize, 65536);

    OpusDecoderAdapter decoder;
    ASSERT_TRUE(decoder.initialize(48000, 2));

    // Decode a packet from the middle of the stream (past any encoder warmup)
    size_t midIdx = pkts.audioData.size() / 2;
    ASSERT_GT(pkts.audioData.size(), midIdx);

    std::vector<float> pcm(960 * 2);
    int frames = decoder.decode(pkts.audioData[midIdx].data(),
                                 pkts.audioData[midIdx].size(),
                                 pcm.data(), 960);
    ASSERT_GT(frames, 0);

    // 440Hz sine wave — should have nonzero energy
    float energy = 0.0f;
    for (int i = 0; i < frames * 2; ++i) {
        energy += pcm[static_cast<size_t>(i)] * pcm[static_cast<size_t>(i)];
    }
    ASSERT_GT(energy, 0.0f);
}

int main() {
    printf("=== End-to-End Demux Tests ===\n\n");
    // Tests run via static constructors above
    printf("\n--- Results: %d passed, %d failed ---\n", g_tests_passed, g_tests_failed);
    return g_tests_failed > 0 ? 1 : 0;
}
