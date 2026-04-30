// parseBlocks() — iterates clusters/blocks from the current position, emits
// AudioPacket + VideoPacket entries into the result, dedupes against the
// last-emitted PTS, handles wrap-reads via per-packet scratch slots, and
// schedules ring compaction.
//
// Member of WebmDemuxer; class definition lives in WebmDemuxer.h.
#include "WebmDemuxer.h"
#include "DemuxLimits.h"

#include "common/MediaConfig.h"
#include "common/MediaLog.h"

#include "mkvparser/mkvparser.h"

namespace media::demux {

void WebmDemuxer::parseBlocks(DemuxResult& result) {
    if (!segment_) {
        return;
    }

    // DIAG: log cluster_ state on parseBlocks entry, throttled to first ~30 calls.
    {
        static int dbg = 0;
        if (++dbg <= 30) {
            MEDIA_LOG_W("parseBlocks#%d: cluster_=%p EOS=%d count=%ld audioT=%d videoT=%d",
                        dbg,
                        static_cast<const void*>(cluster_),
                        cluster_ ? static_cast<int>(cluster_->EOS()) : -1,
                        static_cast<long>(segment_->GetCount()),
                        trackInfo_.audioTrackNum,
                        trackInfo_.videoTrackNum);
        }
    }

    long long latestConsumedPos = compactOffset_;
    int consecutiveBlockErrors = 0;
    scratchCursor_ = 0;

    while (cluster_ && !cluster_->EOS()) {
        // If we haven't started iterating this cluster's blocks, get the first entry
        if (!blockEntry_) {
            long status = cluster_->GetFirst(blockEntry_);
            if (status < 0) {
                // DIAG
                static int dbg2 = 0;
                if (++dbg2 <= 10) {
                    MEDIA_LOG_W("parseBlocks: GetFirst(blockEntry_) status=%ld (need more data)", status);
                }
                // Need more data for this cluster
                break;
            }
            if (!blockEntry_) {
                // DIAG
                static int dbg3 = 0;
                if (++dbg3 <= 10) {
                    MEDIA_LOG_W("parseBlocks: GetFirst returned status=0 with NULL blockEntry_");
                }
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
                lastEmittedAudioPtsUs_ - ptsUs > config::epoch::kMaxBackwardJumpUs) {
                lastEmittedAudioPtsUs_ = -1;
                lastEmittedVideoPtsUs_ = -1;
            } else if (trackNum == trackInfo_.videoTrackNum &&
                       lastEmittedVideoPtsUs_ >= 0 && ptsUs >= 0 &&
                       lastEmittedVideoPtsUs_ - ptsUs > config::epoch::kMaxBackwardJumpUs) {
                lastEmittedAudioPtsUs_ = -1;
                lastEmittedVideoPtsUs_ = -1;
            }

            for (int f = 0; f < frameCount; ++f) {
                const mkvparser::Block::Frame& frame = block->GetFrame(f);

                // Zero-copy path: get direct pointer into the buffer.
                // Falls back to a per-packet scratch slot when data wraps the
                // ring boundary (dataAt returns nullptr). Each wrap-read lands
                // in its own slot so pkt.data pointers from earlier packets in
                // the same feedData() are not clobbered by later wrap-reads.
                const uint8_t* frameData = readerDataAt(frame.pos, frame.len);
                if (!frameData) {
                    if (frame.len > 0 && static_cast<size_t>(frame.len) <= kMaxVideoFrameSize) {
                        if (scratchCursor_ >= scratchBuffers_.size()) {
                            scratchBuffers_.emplace_back();
                        }
                        auto& slot = scratchBuffers_[scratchCursor_];
                        if (slot.size() < static_cast<size_t>(frame.len)) {
                            slot.resize(static_cast<size_t>(frame.len));
                        }
                        if (activeReader()->Read(frame.pos, static_cast<long>(frame.len),
                                                  slot.data()) == 0) {
                            frameData = slot.data();
                            ++scratchCursor_;
                        }
                    }
                    if (!frameData) {
                        // Frame-read failure inside a batch: make the loss
                        // observable even when the rest of the batch succeeded.
                        parseErrorCount_.fetch_add(1, std::memory_order_relaxed);
                        cumulativeParseErrorCount_.fetch_add(1, std::memory_order_relaxed);
                        ++consecutiveBlockErrors;
                        break;
                    }
                }

                if (trackNum == trackInfo_.audioTrackNum) {
                    if (ptsUs == lastEmittedAudioPtsUs_) continue;
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
                    if (ptsUs == lastEmittedVideoPtsUs_) continue;
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

        // Move to the next cluster
        const mkvparser::Cluster* nextCluster = segment_->GetNext(cluster_);
        if (!nextCluster || nextCluster->EOS()) {
            cluster_ = nextCluster;
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
