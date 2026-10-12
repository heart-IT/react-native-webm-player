// parseBlocks() — iterates clusters/blocks from the current position, emits
// AudioPacket + VideoPacket entries into the result, skips a block already
// emitted before a need-more-data retry, and schedules window compaction.
//
// Member of WebmDemuxer; class definition lives in WebmDemuxer.h.
#include "WebmDemuxer.h"
#include "DemuxLimits.h"
#include "WebmReaders.h"

#include "mkvparser/mkvparser.h"

namespace media::demux {

void WebmDemuxer::parseBlocks(DemuxResult& result) {
    if (!segment_) {
        return;
    }

    // Drain newly available clusters into libwebm's m_clusters list. CRITICAL:
    // Segment::Load() is one-shot — it returns E_PARSE_FAILED if any cluster
    // has already been loaded (mkvparser.cc:1451). The granular operation is
    // Segment::LoadCluster() which has no such guard; calling it in a loop
    // until non-zero loads everything currently reachable. Both wedge
    // recoveries below depend on this behaviour to make m_clusterCount grow
    // as new bytes arrive in Streaming state.
    auto drainNewClusters = [this]() {
        for (;;) {
            long status = segment_->LoadCluster();
            if (status != 0) break;  // 1=no more, <0=need more bytes, 2=loops internally
        }
    };

    // Initial wedge recovery — parseTracks() succeeded on a feed that had no
    // cluster bytes yet (e.g. a remuxer that flushes EBML+Segment+Tracks
    // alone), so libwebm's m_clusterCount==0 and Segment::GetFirst() returned
    // the EOS sentinel.
    if (cluster_ && cluster_->EOS() && segment_->GetCount() == 0) {
        drainNewClusters();
        cluster_ = segment_->GetFirst();
        blockEntry_ = nullptr;
        if (!cluster_ || cluster_->EOS()) {
            return;  // Still no clusters loaded; retry on the next feedData().
        }
    }

    // Sequential wedge recovery — the previous parseBlocks() drained
    // cluster_'s blocks but Segment::GetNext() returned the EOS sentinel
    // (no more clusters were loaded into m_clusters yet). LoadCluster()
    // never runs by itself in Streaming state, so without this retry
    // m_clusterCount would stay frozen and every cluster after the first
    // one would be lost.
    if (cluster_ && !cluster_->EOS() && clusterDrained_) {
        drainNewClusters();
        const mkvparser::Cluster* nextCluster = segment_->GetNext(cluster_);
        if (!nextCluster || nextCluster->EOS()) {
            return;  // Still no next cluster; retry on the next feedData().
        }
        cluster_ = nextCluster;
        blockEntry_ = nullptr;
        clusterDrained_ = false;
    }

    // A cluster is ready but the segment carries no SegmentInfo: mkvparser
    // tolerates the omission until Block::GetTime(), which asserts on it in
    // debug builds and null-derefs in release. Without a timecode scale no
    // timestamp in this stream can ever be computed, so this is a permanent
    // error, not a droppable block. Found by fuzzing a mutated laced stream.
    if (cluster_ && !cluster_->EOS() && !segment_->GetInfo()) {
        parseError_ = "stream has clusters but no SegmentInfo";
        state_ = ParseState::Error;
        result.error = "permanent parse failure: " + parseError_;
        return;
    }

    long long latestConsumedPos = compactOffset_;
    // A feed that ends mid-cluster (every feed of a live stream) leaves
    // through one of the two "need more data" exits below. They must still
    // reach the compaction schedule at the end: returning from them left the
    // window untrimmed, so it filled at kMaxBufferSize after ~80 s of
    // broadcast and every later feed was dropped (iPhone 11, 2026-10-12).
    bool needMoreData = false;

    while (cluster_ && !cluster_->EOS()) {
        // Permanent-wedge guard: compaction never trims past the current
        // cluster's own start (the safePos floor below), so at most
        // kMaxBufferSize bytes from that start can ever be resident. A cluster
        // whose declared element size exceeds that can never be fully parsed:
        // append capacity reaches zero, reads inside the tail fail forever,
        // and parse state would stay Streaming over frozen A/V. Surface it as
        // a permanent error per the error table. (Unknown size, -1, is exempt
        // — those clusters are parsed and trimmed block by block.)
        {
            long long declaredSize = cluster_->GetElementSize();
            if (declaredSize > static_cast<long long>(StreamReader::kMaxBufferSize)) {
                parseError_ = "cluster exceeds parse window (" +
                              std::to_string(declaredSize) + " > " +
                              std::to_string(StreamReader::kMaxBufferSize) + " bytes)";
                state_ = ParseState::Error;
                result.error = "permanent parse failure: " + parseError_;
                return;
            }
        }
        // If we haven't started iterating this cluster's blocks, get the first entry
        if (!blockEntry_) {
            long status = cluster_->GetFirst(blockEntry_);
            if (status < 0) {
                // Need more data for this cluster
                break;
            }
        }

        while (blockEntry_ && !blockEntry_->EOS()) {
            const mkvparser::Block* block = blockEntry_->GetBlock();
            if (!block) {
                // Advance to next block entry
                long status = cluster_->GetNext(blockEntry_, blockEntry_);
                if (status < 0) {
                    break;  // Need more data
                }
                continue;
            }

            long long trackNum = block->GetTrackNumber();
            int frameCount = block->GetFrameCount();

            // A cluster-final block is iterable from its header alone while its
            // payload is still in flight: mkvparser completes the cluster
            // without it, so advancing past the block here loses it forever.
            // Checked for every frame before emitting any — emission must be
            // all-or-nothing per block, or the block-level dedup below skips
            // the not-yet-emitted frames on the retry.
            bool payloadPending = false;
            for (int f = 0; f < frameCount; ++f) {
                const mkvparser::Block::Frame& frame = block->GetFrame(f);
                if (readerTotalAvailable() < frame.pos + frame.len) {
                    payloadPending = true;
                    break;
                }
            }
            if (payloadPending) {  // retry this block on the next feed
                needMoreData = true;
                break;
            }

            // Timestamp: cluster time + block time offset, in nanoseconds
            long long timeNs = block->GetTime(cluster_);
            int64_t ptsUs = timeNs / 1000;

            for (int f = 0; f < frameCount; ++f) {
                const mkvparser::Block::Frame& frame = block->GetFrame(f);

                // Zero-copy: a direct pointer into the linear window. There is
                // no fallback read path — StreamReader::Read applies the same
                // bounds as dataAt, so a range dataAt rejects cannot be read at
                // all. Skip: drop-and-continue policy.
                const uint8_t* frameData = readerDataAt(frame.pos, frame.len);
                if (!frameData) break;

                if (trackNum == trackInfo_.audioTrackNum) {
                    // When GetNext() below needs more bytes, the next call
                    // re-visits the block just emitted. Dedup is per block,
                    // decided at the first frame only: in a laced block every
                    // frame carries the block's one PTS, so a per-frame
                    // comparison silently dropped frames 2..n.
                    if (f == 0 && ptsUs == lastEmittedAudioPtsUs_) break;
                    AudioPacket pkt;
                    pkt.data = frameData;
                    pkt.size = static_cast<size_t>(frame.len);
                    pkt.ptsUs = ptsUs;
                    result.audioPackets.push_back(pkt);
                    lastEmittedAudioPtsUs_ = ptsUs;
                } else if (trackNum == trackInfo_.videoTrackNum) {
                    // Same block-level dedup contract as the audio path above.
                    if (f == 0 && ptsUs == lastEmittedVideoPtsUs_) break;
                    VideoPacket pkt;
                    pkt.data = frameData;
                    pkt.size = static_cast<size_t>(frame.len);
                    pkt.ptsUs = ptsUs;
                    pkt.isKeyFrame = block->IsKey();
                    result.videoPackets.push_back(pkt);
                    lastEmittedVideoPtsUs_ = ptsUs;
                }

                // Track the end of this frame for compaction
                long long frameEnd = frame.pos + frame.len;
                if (frameEnd > latestConsumedPos) {
                    latestConsumedPos = frameEnd;
                }
            }

            // Advance to next block entry
            const mkvparser::BlockEntry* nextEntry = nullptr;
            long status = cluster_->GetNext(blockEntry_, nextEntry);
            if (status < 0) {  // the next block header is still in flight
                needMoreData = true;
                break;
            }
            blockEntry_ = nextEntry;
        }
        if (needMoreData) break;

        // Move to the next cluster. parseTracks() was the only place that
        // initially ran Segment::LoadCluster(), so in Streaming state
        // m_clusterCount can only grow via this path — drain any clusters
        // that arrived since the last attempt before giving up.
        const mkvparser::Cluster* nextCluster = segment_->GetNext(cluster_);
        if (!nextCluster || nextCluster->EOS()) {
            drainNewClusters();
            nextCluster = segment_->GetNext(cluster_);
        }
        if (!nextCluster || nextCluster->EOS()) {
            // Still no next cluster. Keep cluster_ pointing at the last real
            // cluster (NOT the EOS sentinel) so the next parseBlocks() call
            // can retry advancing once more bytes arrive. The clusterDrained_
            // flag prevents the outer while from re-iterating its blocks.
            clusterDrained_ = true;
            blockEntry_ = nullptr;
            break;
        }

        // Check if the next cluster's data is available. The loop-top guard
        // catches an oversized declared size on the next iteration (this call
        // or the next feedData), so waiting here cannot wedge permanently.
        long long clusterPos = nextCluster->GetPosition();
        long long clusterSize = nextCluster->GetElementSize();
        // If element size is unknown yet, just try — GetFirst will fail if data is missing
        if (clusterSize > 0 && readerTotalAvailable() < clusterPos + clusterSize &&
            clusterSize <= static_cast<long long>(StreamReader::kMaxBufferSize)) {
            // Not enough data for this cluster yet; stay put
            cluster_ = nextCluster;
            blockEntry_ = nullptr;
            break;
        }

        cluster_ = nextCluster;
        blockEntry_ = nullptr;
    }

    // Schedule compaction for the start of the next feedData() call.
    // Deferring compaction ensures packet data pointers in the returned
    // DemuxResult remain valid until the caller processes them.
    if (latestConsumedPos > compactOffset_) {
        compactOffset_ = latestConsumedPos;
        if (cluster_ && !cluster_->EOS()) {
            long long safePos = cluster_->GetPosition();
            if (safePos > 0) {
                pendingCompactPos_ = safePos;
            }
        } else {
            pendingCompactPos_ = latestConsumedPos;
        }
    }
}

}  // namespace media::demux
