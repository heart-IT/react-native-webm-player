// End-to-end test: feeds a real WebM stream (VP9 + Opus) through the full
// demux pipeline and validates packet correctness.
//
// Build: cmake -B tests/sanitizer/build-asan -S tests/sanitizer -DSANITIZER=address
//        cmake --build tests/sanitizer/build-asan --target test_e2e_demux
// Run:   tests/sanitizer/build-asan/test_e2e_demux

#include "test_common.h"
#include <vector>
#include <algorithm>
#include <chrono>
#include <thread>

#include "demux/WebmDemuxer.h"
#include "common/IngestRingBuffer.h"
#include "video/VP9HeaderParser.h"
#include "OpusDecoderAdapter.h"
#include "webm_fixture.h"

using namespace media;
using namespace media::demux;

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

    auto info = demuxer.trackInfoSnapshot();
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
    auto header = demuxer.streamHeader();
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
    while (fed < kTotalFiller) {
        demuxer.feedData(filler.data(), filler.size());
        fed += filler.size();
    }

    auto metrics = demuxer.demuxerMetrics();

    // Buffer must be capped at kMaxBufferSize
    ASSERT_LE(metrics.bufferBytes, kMaxBuffer);

    // Overflow or partial drops must have been detected
    ASSERT_GT(metrics.overflowCount + metrics.partialDropCount, 0u);
}

// Ring-mode parity: output must match the stream-mode baseline byte-for-byte.
// This catches broken scratch-slot management (the P0 pre-fix signature was
// packet bytes getting clobbered across a wrap). Wrap-path coverage itself
// is provided by fuzz_demuxer (100k iterations under ASan with crafted +
// random ring states that exercise dataAt() returning nullptr).
TEST(ring_mode_matches_stream_mode_baseline_byte_for_byte) {
    WebmDemuxer stream;
    auto baseline = feedAll(stream, fixture::kWebmData, fixture::kWebmDataSize, 4096);
    ASSERT_TRUE(!baseline.hadError);

    // Power-of-2 ring large enough to comfortably hold the full fixture.
    // Wrap coverage is exercised separately by fuzz_demuxer.
    constexpr size_t kRingCapacity = 64 * 1024;
    IngestRingBuffer ring(kRingCapacity);
    WebmDemuxer ringDemux;
    ringDemux.setRingBuffer(&ring);

    std::vector<std::vector<uint8_t>> audioRing;
    std::vector<std::vector<uint8_t>> videoRing;

    auto drain = [&]() {
        const auto& result = ringDemux.feedData(nullptr, 0);
        for (const auto& pkt : result.audioPackets) {
            audioRing.emplace_back(pkt.data, pkt.data + pkt.size);
        }
        for (const auto& pkt : result.videoPackets) {
            videoRing.emplace_back(pkt.data, pkt.data + pkt.size);
        }
        ring.setDecodeRetainPos(ring.currentWritePos());
    };

    constexpr size_t kChunk = 4096;
    size_t offset = 0;
    while (offset < fixture::kWebmDataSize) {
        size_t chunk = std::min(kChunk, fixture::kWebmDataSize - offset);
        ASSERT_TRUE(ring.write(fixture::kWebmData + offset, chunk));
        offset += chunk;
        drain();
    }
    drain();

    ASSERT_EQ(audioRing.size(), baseline.audioData.size());
    ASSERT_EQ(videoRing.size(), baseline.videoData.size());
    for (size_t i = 0; i < audioRing.size(); ++i) {
        ASSERT_EQ(audioRing[i].size(), baseline.audioData[i].size());
        ASSERT_TRUE(audioRing[i] == baseline.audioData[i]);
    }
    for (size_t i = 0; i < videoRing.size(); ++i) {
        ASSERT_EQ(videoRing[i].size(), baseline.videoData[i].size());
        ASSERT_TRUE(videoRing[i] == baseline.videoData[i]);
    }
}

TEST(ingest_ring_writeRejects_increments_when_full) {
    IngestRingBuffer ring(1024);
    // Fill ring.
    std::vector<uint8_t> chunk(1024, 0xA5);
    ASSERT_TRUE(ring.write(chunk.data(), chunk.size()));
    ASSERT_EQ(ring.writeRejects(), 0u);
    // Subsequent writes reject until compaction.
    ASSERT_TRUE(!ring.write(chunk.data(), 1));
    ASSERT_EQ(ring.writeRejects(), 1u);
    ASSERT_TRUE(!ring.write(chunk.data(), 512));
    ASSERT_EQ(ring.writeRejects(), 2u);
}

TEST(demuxer_split_drop_counters_separate_from_aggregate) {
    // Empty-state baseline — all counters zero.
    WebmDemuxer demuxer;
    auto m = demuxer.demuxerMetrics();
    ASSERT_EQ(m.partialDropCount, 0u);
    ASSERT_EQ(m.oversizedFrameDrops, 0u);
    ASSERT_EQ(m.packetCapDrops, 0u);
    ASSERT_EQ(m.appendBackpressureDrops, 0u);

    // Flood stream-mode demuxer until append backpressure kicks in.
    feedAll(demuxer, fixture::kWebmData, fixture::kWebmDataSize, 65536);
    constexpr size_t kFiller = 3 * 1024 * 1024;  // >> StreamReader::kMaxBufferSize (2 MiB)
    std::vector<uint8_t> filler(65536, 0xBB);
    size_t fed = 0;
    while (fed < kFiller) {
        demuxer.feedData(filler.data(), filler.size());
        fed += filler.size();
    }
    m = demuxer.demuxerMetrics();
    // Append-backpressure counter bumped at least once; partialDropCount is the sum.
    ASSERT_GT(m.appendBackpressureDrops, 0u);
    ASSERT_GE(m.partialDropCount,
              m.oversizedFrameDrops + m.packetCapDrops + m.appendBackpressureDrops);
}

TEST(demuxer_cumulative_counters_survive_reset) {
    WebmDemuxer demuxer;
    auto initial = demuxer.demuxerMetrics();
    ASSERT_EQ(initial.parseErrorCount, 0u);
    ASSERT_EQ(initial.cumulativeParseErrorCount, 0u);
    ASSERT_EQ(initial.sessionResetCount, 0u);

    // Drive the demuxer into Error state so parse errors actually bump.
    std::vector<uint8_t> garbage(1024, 0xCC);
    for (int i = 0; i < 20; ++i) {
        demuxer.feedData(garbage.data(), garbage.size());
        if (demuxer.parseState() == ParseState::Error) break;
    }
    ASSERT_EQ(demuxer.parseState(), ParseState::Error);

    auto afterError = demuxer.demuxerMetrics();
    ASSERT_GT(afterError.parseErrorCount, 0u);
    ASSERT_EQ(afterError.parseErrorCount, afterError.cumulativeParseErrorCount);

    uint64_t errsBeforeReset = afterError.cumulativeParseErrorCount;
    demuxer.reset();

    auto afterReset = demuxer.demuxerMetrics();
    // Session counters zeroed, cumulative preserved, sessionResetCount bumped.
    ASSERT_EQ(afterReset.parseErrorCount, 0u);
    ASSERT_EQ(afterReset.cumulativeParseErrorCount, errsBeforeReset);
    ASSERT_EQ(afterReset.sessionResetCount, 1u);

    // A second round of errors + reset: cumulative keeps growing monotonically.
    for (int i = 0; i < 20; ++i) {
        demuxer.feedData(garbage.data(), garbage.size());
        if (demuxer.parseState() == ParseState::Error) break;
    }
    auto afterSecondError = demuxer.demuxerMetrics();
    ASSERT_GT(afterSecondError.cumulativeParseErrorCount, errsBeforeReset);

    demuxer.reset();
    auto afterSecondReset = demuxer.demuxerMetrics();
    ASSERT_EQ(afterSecondReset.sessionResetCount, 2u);
    ASSERT_EQ(afterSecondReset.cumulativeParseErrorCount,
              afterSecondError.cumulativeParseErrorCount);
}

TEST(demuxer_timeInErrorMs_positive_after_permanent_error) {
    WebmDemuxer demuxer;
    // Feed persistent garbage (non-EBML) until parse retries exhaust and the
    // demuxer transitions to permanent Error state. kMaxParseRetries=3.
    std::vector<uint8_t> garbage(1024, 0xCC);
    for (int i = 0; i < 20; ++i) {
        demuxer.feedData(garbage.data(), garbage.size());
        if (demuxer.parseState() == ParseState::Error) break;
    }
    ASSERT_EQ(demuxer.parseState(), ParseState::Error);

    // Sleep long enough that (nowUs-entry)/1000 is safely >= 1 ms even under
    // scheduler jitter from sanitizers.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto m = demuxer.demuxerMetrics();
    ASSERT_EQ(m.parseState, ParseState::Error);
    ASSERT_GT(m.timeInErrorMs, 0);

    // After reset, state returns to WaitingForEBML and the counter must zero.
    demuxer.reset();
    auto after = demuxer.demuxerMetrics();
    ASSERT_EQ(after.parseState, ParseState::WaitingForEBML);
    ASSERT_EQ(after.timeInErrorMs, 0);
}

// Regression: feed#0 = EBML+Segment+Tracks header only (no cluster bytes
// yet), feed#1+ = clusters. Pre-fix, parseTracks() succeeded with libwebm's
// m_clusterCount==0, GetFirst() returned the EOS sentinel, and cluster_
// stayed wedged on it forever — zero packets emitted regardless of how
// many cluster bytes were fed afterward. Mirrors the production wedge on
// remuxers that flush the WebM header in its own write.
TEST(header_only_first_chunk_then_clusters_does_not_wedge) {
    size_t firstClusterOffset = 0;
    for (size_t i = 0; i + 4 < fixture::kWebmDataSize; ++i) {
        if (fixture::kWebmData[i] == 0x1F &&
            fixture::kWebmData[i + 1] == 0x43 &&
            fixture::kWebmData[i + 2] == 0xB6 &&
            fixture::kWebmData[i + 3] == 0x75) {
            firstClusterOffset = i;
            break;
        }
    }
    ASSERT_TRUE(firstClusterOffset > 0);

    constexpr size_t kRingCapacity = 1 << 21;
    ASSERT_TRUE(fixture::kWebmDataSize <= kRingCapacity);

    IngestRingBuffer ring(kRingCapacity);
    WebmDemuxer demuxer;
    demuxer.setRingBuffer(&ring);

    ASSERT_TRUE(ring.write(fixture::kWebmData, firstClusterOffset));
    const auto& r1 = demuxer.feedData(nullptr, firstClusterOffset);
    ASSERT_TRUE(r1.audioPackets.empty());
    ASSERT_TRUE(r1.videoPackets.empty());

    size_t audio = 0, video = 0;
    const size_t remaining = fixture::kWebmDataSize - firstClusterOffset;
    ASSERT_TRUE(ring.write(fixture::kWebmData + firstClusterOffset, remaining));
    const auto& r2 = demuxer.feedData(nullptr, remaining);
    audio += r2.audioPackets.size();
    video += r2.videoPackets.size();
    ring.setDecodeRetainPos(ring.currentWritePos());
    const auto& r3 = demuxer.feedData(nullptr, 0);
    audio += r3.audioPackets.size();
    video += r3.videoPackets.size();

    ASSERT_TRUE(audio > 0 || video > 0);
}

TEST_MAIN("End-to-End Demux Tests")
