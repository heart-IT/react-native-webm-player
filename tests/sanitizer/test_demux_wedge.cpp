// Regression tests for two parseBlocks() wedges that lost every cluster
// after the first one in production:
//   (1) initial wedge — parseTracks() runs before any cluster bytes are in
//       the buffer, GetFirst() returns the EOS sentinel and cluster_ stays
//       stuck on it across subsequent feeds;
//   (2) sequential wedge — Segment::Load() is one-shot (mkvparser.cc:1451)
//       so post-parseTracks no new cluster ever reaches m_clusters; once
//       parseBlocks() exhausts the initial cluster, GetNext() returns the
//       EOS sentinel and the demuxer overwrites cluster_ with it.
// Fix: parseBlocks() drives Segment::LoadCluster() (no one-shot guard) on
// both paths and stops overwriting cluster_ when GetNext() returns EOS.

#include "test_common.h"
#include <vector>
#include <algorithm>

#include "demux/WebmDemuxer.h"
#include "demux/WebmReaders.h"
#include "common/IngestRingBuffer.h"
#include "webm_fixture.h"
#include "mkvparser/mkvparser.h"

using namespace media;
using namespace media::demux;

// Initial wedge: feed only the EBML+Segment+Tracks header on the first
// feedData(), then the rest. Pre-fix this stayed at zero packets forever.
TEST(header_only_first_chunk_then_clusters_does_not_wedge) {
    size_t firstClusterOffset = 0;
    for (size_t i = 0; i + 4 < fixture::kWebmDataSize; ++i) {
        if (fixture::kWebmData[i] == 0x1F && fixture::kWebmData[i + 1] == 0x43 &&
            fixture::kWebmData[i + 2] == 0xB6 && fixture::kWebmData[i + 3] == 0x75) {
            firstClusterOffset = i;
            break;
        }
    }
    ASSERT_TRUE(firstClusterOffset > 0);

    constexpr size_t kRingCapacity = 1 << 21;
    IngestRingBuffer ring(kRingCapacity);
    WebmDemuxer demuxer;
    demuxer.setRingBuffer(&ring);

    ASSERT_TRUE(ring.write(fixture::kWebmData, firstClusterOffset));
    const auto& r1 = demuxer.feedData(nullptr, firstClusterOffset);
    ASSERT_TRUE(r1.audioPackets.empty() && r1.videoPackets.empty());

    const size_t remaining = fixture::kWebmDataSize - firstClusterOffset;
    ASSERT_TRUE(ring.write(fixture::kWebmData + firstClusterOffset, remaining));
    const auto& r2 = demuxer.feedData(nullptr, remaining);
    size_t total = r2.audioPackets.size() + r2.videoPackets.size();
    ring.setDecodeRetainPos(ring.currentWritePos());
    const auto& r3 = demuxer.feedData(nullptr, 0);
    total += r3.audioPackets.size() + r3.videoPackets.size();
    ASSERT_TRUE(total > 0);
}

// Sequential wedge: feed exactly one cluster's bytes per feedData() call,
// drain in between. Pre-fix, parseTracks's one-shot Segment::Load() loaded
// only cluster #1; cluster #2's bytes never reached m_clusters and its
// audio/video packets were silently lost.
TEST(per_cluster_feeds_load_subsequent_clusters) {
    // Locate cluster offsets so we can split exactly between them.
    std::vector<long long> clusterPositions;
    {
        StreamReader r;
        r.append(fixture::kWebmData, fixture::kWebmDataSize);
        long long pos = 0;
        mkvparser::EBMLHeader ebml;
        ASSERT_EQ(ebml.Parse(&r, pos), 0);
        mkvparser::Segment* raw = nullptr;
        ASSERT_EQ(mkvparser::Segment::CreateInstance(&r, pos, raw), 0);
        std::unique_ptr<mkvparser::Segment> segment(raw);
        segment->Load();  // -3 in streaming mode is fine
        for (const mkvparser::Cluster* c = segment->GetFirst();
             c && !c->EOS(); c = segment->GetNext(c))
            clusterPositions.push_back(c->GetPosition());
    }
    ASSERT_TRUE(clusterPositions.size() >= 2u);

    constexpr size_t kRingCapacity = 1 << 21;
    IngestRingBuffer ring(kRingCapacity);
    WebmDemuxer demuxer;
    demuxer.setRingBuffer(&ring);

    size_t emittedAudio = 0, emittedVideo = 0;
    size_t offset = 0;
    auto feedTo = [&](size_t end) {
        if (end <= offset) return;
        size_t len = end - offset;
        ASSERT_TRUE(ring.write(fixture::kWebmData + offset, len));
        const auto& r = demuxer.feedData(nullptr, len);
        emittedAudio += r.audioPackets.size();
        emittedVideo += r.videoPackets.size();
        ring.setDecodeRetainPos(ring.currentWritePos());
        offset = end;
    };

    // Step 1: feed bytes up to (but NOT including) the second cluster.
    // After this drain, m_clusterCount must be 1 — only cluster #1 was loadable.
    feedTo(static_cast<size_t>(clusterPositions[1]));
    const size_t afterStep1Audio = emittedAudio;

    // Step 2: feed remaining bytes (cluster #2 onwards). The pre-fix one-shot
    // Segment::Load() returned E_PARSE_FAILED here, leaving cluster #2 invisible.
    feedTo(fixture::kWebmDataSize);
    const auto& tail = demuxer.feedData(nullptr, 0);
    emittedAudio += tail.audioPackets.size();
    emittedVideo += tail.videoPackets.size();

    // Cluster #2 contributes audio packets that pre-fix were dropped. The
    // exact count varies with fixture, so just require at least one audio
    // packet from cluster #2 to land — which never happens pre-fix.
    ASSERT_GT(emittedAudio, afterStep1Audio);
}

TEST_MAIN("Demuxer Wedge Regression Tests")
