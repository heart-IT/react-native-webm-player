// End-to-end test: feeds a real WebM stream (VP9 + Opus) through the full
// demux pipeline and validates packet correctness.
//
// Build: cmake -B tests/sanitizer/build-asan -S tests/sanitizer -DSANITIZER=address
//        cmake --build tests/sanitizer/build-asan --target test_e2e_demux
// Run:   tests/sanitizer/build-asan/test_e2e_demux

#include "test_common.h"
#include <vector>
#include <algorithm>

#include "demux/WebmDemuxer.h"
#include "video/VP9HeaderParser.h"
#include "OpusDecoderAdapter.h"
#include "fixtures/webm_fixture.h"
#include "common/MediaLog.h"

namespace {
// Several tests deliberately overflow the StreamReader window; on host builds
// MediaLog writes those warnings to stderr and drowns the results.
struct SilenceLogs {
  SilenceLogs() {
    media::log::minLevel().store(media::log::Level::Error,
                                 std::memory_order_relaxed);
  }
};
SilenceLogs g_silence_logs;
}  // namespace

using namespace media;
using namespace media::demux;

// Helper: feed entire fixture and collect all packets
struct CollectedPackets {
    std::vector<std::vector<uint8_t>> audioData;
    std::vector<int64_t> audioPts;
    std::vector<std::vector<uint8_t>> videoData;
    std::vector<int64_t> videoPts;
    std::vector<bool> videoKeyFrame;
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
        }
        for (const auto& pkt : result.videoPackets) {
            out.videoData.emplace_back(pkt.data, pkt.data + pkt.size);
            out.videoPts.push_back(pkt.ptsUs);
            out.videoKeyFrame.push_back(pkt.isKeyFrame);
        }
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
    ASSERT_EQ(info.videoWidth, 320);
    ASSERT_EQ(info.videoHeight, 240);
    ASSERT_EQ(info.videoFrameDurationNs, 33333333);  // DefaultDuration, 30 fps
    ASSERT_TRUE(info.audioTrackNum >= 0);
    ASSERT_TRUE(info.videoTrackNum >= 0);
    // The iOS engine configures the Opus decoder from these, so a silent zero
    // here would mean opus_decoder_create fails at runtime.
    ASSERT_EQ(info.audioSampleRate, 48000);
    ASSERT_EQ(info.audioChannels, 2);
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

TEST(demuxer_reaches_streaming_state) {
    WebmDemuxer demuxer;
    feedAll(demuxer, fixture::kWebmData, fixture::kWebmDataSize, 4096);
    ASSERT_EQ(demuxer.parseState(), ParseState::Streaming);
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

TEST(stream_reader_rejects_data_beyond_max_buffer_size) {
    // StreamReader::kMaxBufferSize is 2MB. First reach Streaming state with
    // valid data, then flood with filler to exceed 2MB and verify overflow.
    WebmDemuxer demuxer;

    // Feed valid WebM to reach Streaming state (compaction only runs during parseBlocks)
    feedAll(demuxer, fixture::kWebmData, fixture::kWebmDataSize, 65536);
    ASSERT_EQ(demuxer.parseState(), ParseState::Streaming);

    // Now flood with filler data. The demuxer is in Streaming state so it will
    // call parseBlocks() each time (which won't find valid clusters), but the
    // StreamReader will accumulate bytes until the 2MB cap.
    constexpr size_t kChunkSize = 65536;
    constexpr size_t kMaxBuffer = 2 * 1024 * 1024;  // Must match StreamReader::kMaxBufferSize
    constexpr size_t kTotalFiller = kMaxBuffer + 4 * kChunkSize;

    std::vector<uint8_t> filler(kChunkSize, 0xAA);
    size_t fed = 0;
    size_t packets = 0;
    while (fed < kTotalFiller) {
        const auto& result = demuxer.feedData(filler.data(), filler.size());
        packets += result.audioPackets.size() + result.videoPackets.size();
        fed += filler.size();
        ASSERT_LE(demuxer.bufferBytes(), kMaxBuffer);
    }

    // Filler is not a cluster: nothing is emitted and the state holds.
    ASSERT_EQ(packets, size_t(0));
    ASSERT_EQ(demuxer.parseState(), ParseState::Streaming);
}

TEST_MAIN("End-to-End Demux Tests")
