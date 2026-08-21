// parseBlocks() — iterates clusters/blocks from the current position, emits
// AudioPacket + VideoPacket entries into the result, dedupes duplicate blocks
// against the last-emitted PTS, and schedules window compaction.
//
// Member of WebmDemuxer; class definition lives in WebmDemuxer.h.
#include "WebmDemuxer.h"
#include "DemuxLimits.h"


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
        if (result.newClusterPositions.size() < kMaxClustersPerFeed) {
            result.newClusterPositions.push_back(cluster_->GetPosition());
        }
        ++result.newClusterCount;
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
        if (result.newClusterPositions.size() < kMaxClustersPerFeed) {
            result.newClusterPositions.push_back(cluster_->GetPosition());
        }
        ++result.newClusterCount;
    }

    long long latestConsumedPos = compactOffset_;
    int consecutiveBlockErrors = 0;

    while (cluster_ && !cluster_->EOS()) {
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
                ++consecutiveBlockErrors;
                // Advance to next block entry
                long status = cluster_->GetNext(blockEntry_, blockEntry_);
                if (status < 0) {
                    break;  // Need more data
                }
                continue;
            }
            consecutiveBlockErrors = 0;

            long long trackNum = block->GetTrackNumber();
            int frameCount = block->GetFrameCount();

            // Timestamp: cluster time + block time offset, in nanoseconds
            long long timeNs = block->GetTime(cluster_);
            int64_t ptsUs = timeNs / 1000;

            // Detect large backward PTS jump (broadcaster restart within same segment).
            // Reset dedup state so early frames after restart are not silently dropped.
            // Threshold shared with AudioDecodeChannel's discontinuity detection so the two
            // stay in lockstep — drift between them would cause silent dedup of valid frames.
            if (trackNum == trackInfo_.audioTrackNum &&
                lastEmittedAudioPtsUs_ >= 0 && ptsUs >= 0 &&
                lastEmittedAudioPtsUs_ - ptsUs > kMaxBackwardJumpUs) {
                lastEmittedAudioPtsUs_ = -1;
                lastEmittedVideoPtsUs_ = -1;
            } else if (trackNum == trackInfo_.videoTrackNum &&
                       lastEmittedVideoPtsUs_ >= 0 && ptsUs >= 0 &&
                       lastEmittedVideoPtsUs_ - ptsUs > kMaxBackwardJumpUs) {
                lastEmittedAudioPtsUs_ = -1;
                lastEmittedVideoPtsUs_ = -1;
            }

            for (int f = 0; f < frameCount; ++f) {
                const mkvparser::Block::Frame& frame = block->GetFrame(f);

                // Zero-copy: a direct pointer into the linear window. There is
                // no fallback read path — StreamReader::Read applies the same
                // bounds as dataAt, so a range dataAt rejects cannot be read at
                // all. Count the loss and skip: drop-and-continue policy.
                const uint8_t* frameData = readerDataAt(frame.pos, frame.len);
                if (!frameData) {
                    parseErrorCount_.fetch_add(1, std::memory_order_relaxed);
                    cumulativeParseErrorCount_.fetch_add(1, std::memory_order_relaxed);
                    ++consecutiveBlockErrors;
                    break;
                }

                if (trackNum == trackInfo_.audioTrackNum) {
                    // Dedup is per block (Hypercore delivery retries), decided at
                    // the first frame only: in a laced block every frame carries
                    // the block's one PTS, so a per-frame comparison against the
                    // just-updated last-emitted PTS silently dropped frames 2..n.
                    if (f == 0 && ptsUs == lastEmittedAudioPtsUs_) break;
                    if (result.audioPackets.size() >= kMaxAudioPacketsPerFeed) {
                        // Cap reached: the remaining blocks in this feed are
                        // silently lost (we cannot hold them and the iterator
                        // advances). Make the drop observable via metrics.
                        partialDropCount_.fetch_add(1, std::memory_order_relaxed);
                        packetCapDrops_.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    AudioPacket pkt;
                    pkt.data = frameData;
                    pkt.size = static_cast<size_t>(frame.len);
                    pkt.ptsUs = ptsUs;
                    pkt.durationUs = kDefaultAudioDurationUs;
                    pkt.absOffset = frame.pos;
                    result.audioPackets.push_back(pkt);
                    audioPacketsEmitted_.fetch_add(1, std::memory_order_relaxed);
                    lastEmittedAudioPtsUs_ = ptsUs;
                } else if (trackNum == trackInfo_.videoTrackNum) {
                    auto frameSize = static_cast<size_t>(frame.len);
                    if (frameSize > kMaxVideoFrameSize) {
                        partialDropCount_.fetch_add(1, std::memory_order_relaxed);
                        oversizedFrameDrops_.fetch_add(1, std::memory_order_relaxed);
                        continue;  // Reject oversized frames
                    }
                    // Same block-level dedup contract as the audio path above.
                    if (f == 0 && ptsUs == lastEmittedVideoPtsUs_) break;
                    if (result.videoPackets.size() >= kMaxVideoPacketsPerFeed) {
                        partialDropCount_.fetch_add(1, std::memory_order_relaxed);
                        packetCapDrops_.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    VideoPacket pkt;
                    pkt.data = frameData;
                    pkt.size = frameSize;
                    pkt.ptsUs = ptsUs;
                    pkt.isKeyFrame = block->IsKey();
                    pkt.absOffset = frame.pos;
                    result.videoPackets.push_back(pkt);
                    videoPacketsEmitted_.fetch_add(1, std::memory_order_relaxed);
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
            if (status < 0) {
                // Need more data for next block
                return;
            }
            blockEntry_ = nextEntry;
        }

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

        // Check if the next cluster's data is available
        long long clusterPos = nextCluster->GetPosition();
        long long clusterSize = nextCluster->GetElementSize();
        // If element size is unknown yet, just try — GetFirst will fail if data is missing
        if (clusterSize > 0 && readerTotalAvailable() < clusterPos + clusterSize) {
            // Not enough data for this cluster yet; stay put
            cluster_ = nextCluster;
            blockEntry_ = nullptr;
            break;
        }

        cluster_ = nextCluster;
        blockEntry_ = nullptr;
        if (result.newClusterPositions.size() < kMaxClustersPerFeed) {
            result.newClusterPositions.push_back(clusterPos);
        }
        ++result.newClusterCount;
    }

    // Report if blocks are consistently unparseable despite data being available
    if (consecutiveBlockErrors >= kMaxConsecutiveBlockErrors && result.audioPackets.empty() && result.videoPackets.empty()) {
        result.error = "block parse stall: consecutive unparseable blocks";
        blockStallCount_.fetch_add(1, std::memory_order_relaxed);
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
