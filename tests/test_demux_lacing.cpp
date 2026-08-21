// Laced-block demux tests: a Xiph-laced SimpleBlock must emit every laced
// frame, not just the first. Pins the block-level dedup contract (dedup is
// decided at frame 0 only — a per-frame PTS comparison silently dropped
// frames 2..n, since all laced frames carry the block's one timestamp).
//
// Build/run: part of the sanitizer suites (see tests/CMakeLists.txt).

#include "test_common.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "demux/WebmDemuxer.h"
#include "webm_synth.h"

using namespace media::demux;

namespace {

std::vector<uint8_t> lacedStream() {
    std::vector<uint8_t> s(webm_synth::kMinimalWebmHeader,
                           webm_synth::kMinimalWebmHeader +
                               sizeof(webm_synth::kMinimalWebmHeader));
    s.insert(s.end(), webm_synth::kXiphLacedCluster,
             webm_synth::kXiphLacedCluster + sizeof(webm_synth::kXiphLacedCluster));
    s.insert(s.end(), webm_synth::kXiphLacedCluster2,
             webm_synth::kXiphLacedCluster2 + sizeof(webm_synth::kXiphLacedCluster2));
    return s;
}

struct Collected {
    std::vector<std::vector<uint8_t>> data;
    std::vector<int64_t> pts;
};

Collected feedAll(WebmDemuxer& demuxer, const std::vector<uint8_t>& stream,
                  size_t chunkSize) {
    Collected out;
    size_t offset = 0;
    while (offset < stream.size()) {
        size_t chunk = std::min(chunkSize, stream.size() - offset);
        const auto& result = demuxer.feedData(stream.data() + offset, chunk);
        for (const auto& pkt : result.audioPackets) {
            out.data.emplace_back(pkt.data, pkt.data + pkt.size);
            out.pts.push_back(pkt.ptsUs);
        }
        offset += chunk;
    }
    return out;
}

}  // namespace

TEST(xiph_laced_block_emits_all_three_frames) {
    WebmDemuxer demuxer;
    auto stream = lacedStream();
    auto pkts = feedAll(demuxer, stream, stream.size());

    ASSERT_EQ(demuxer.parseState(), ParseState::Streaming);
    // 2 clusters x 3 laced frames each.
    ASSERT_EQ(pkts.data.size(), 6u);

    // Frames carry the exact laced payloads, in lace order.
    ASSERT_TRUE(pkts.data[0] == (std::vector<uint8_t>{0x01, 0x02, 0x03}));
    ASSERT_TRUE(pkts.data[1] == (std::vector<uint8_t>{0x11, 0x12, 0x13, 0x14, 0x15}));
    ASSERT_TRUE(pkts.data[2] == (std::vector<uint8_t>{0x21, 0x22, 0x23, 0x24}));
    ASSERT_TRUE(pkts.data[3] == (std::vector<uint8_t>{0x31, 0x32, 0x33}));
    ASSERT_TRUE(pkts.data[4] == (std::vector<uint8_t>{0x41, 0x42, 0x43, 0x44, 0x45}));
    ASSERT_TRUE(pkts.data[5] == (std::vector<uint8_t>{0x51, 0x52, 0x53, 0x54}));

    // The parser does not synthesize per-frame spacing: every laced frame
    // carries its block's timestamp (cluster timecodes 0 ms and 20 ms).
    ASSERT_EQ(pkts.pts[0], 0);
    ASSERT_EQ(pkts.pts[1], 0);
    ASSERT_EQ(pkts.pts[2], 0);
    ASSERT_EQ(pkts.pts[3], 20000);
    ASSERT_EQ(pkts.pts[4], 20000);
    ASSERT_EQ(pkts.pts[5], 20000);
}

TEST(xiph_laced_block_survives_byte_at_a_time_feeding) {
    WebmDemuxer demuxer;
    auto stream = lacedStream();
    auto pkts = feedAll(demuxer, stream, 1);

    ASSERT_EQ(demuxer.parseState(), ParseState::Streaming);
    ASSERT_EQ(pkts.data.size(), 6u);
    ASSERT_TRUE(pkts.data[0] == (std::vector<uint8_t>{0x01, 0x02, 0x03}));
    ASSERT_TRUE(pkts.data[5] == (std::vector<uint8_t>{0x51, 0x52, 0x53, 0x54}));
}

TEST_MAIN("Laced-Block Demux Tests")
