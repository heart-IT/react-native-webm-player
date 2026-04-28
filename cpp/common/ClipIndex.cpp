// ClipIndex implementation. See ClipIndex.h for the threading contract and
// the invariants this code maintains across the ingest thread / background
// extraction thread boundary.
#include "ClipIndex.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

#include "IngestRingBuffer.h"
#include "MediaConfig.h"
#include "MediaLog.h"
#include "Platform.h"

namespace media {

ClipIndex::ClipIndex() noexcept {
    clusters_.resize(config::clip::kMaxClusters);
}

void ClipIndex::setRingBuffer(IngestRingBuffer* ring) noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    ring_ = ring;
}

void ClipIndex::setEnabled(bool enabled) noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    bool was = enabled_.load(std::memory_order_relaxed);
    if (enabled && !was) {
        clusterCount_ = 0;
        clusterWriteIdx_ = 0;
        hasPendingCluster_ = false;
    } else if (!enabled && was) {
        clusterCount_ = 0;
        clusterWriteIdx_ = 0;
        if (ring_) ring_->setRetainPos(0);
    }
    enabled_.store(enabled, std::memory_order_release);
}

void ClipIndex::setStreamHeader(const uint8_t* data, size_t len) noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    streamHeader_.assign(data, data + len);
}

bool ClipIndex::hasStreamHeader() const noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    return !streamHeader_.empty();
}

std::vector<uint8_t> ClipIndex::streamHeader() const noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    return streamHeader_;
}

void ClipIndex::onNewCluster(long long absOffset) noexcept {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lk(mutex_);
    sealPendingCluster(absOffset);
    hasPendingCluster_ = true;
    pendingClusterAbsOffset_ = absOffset;
    pendingHasKeyFrame_ = false;
    pendingFirstPts_ = std::numeric_limits<int64_t>::min();
}

void ClipIndex::onKeyFrame(int64_t ptsUs) noexcept {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lk(mutex_);
    pendingHasKeyFrame_ = true;
    if (pendingFirstPts_ == std::numeric_limits<int64_t>::min()) {
        pendingFirstPts_ = ptsUs;
    }
}

void ClipIndex::onBlockPts(int64_t ptsUs) noexcept {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lk(mutex_);
    if (pendingFirstPts_ == std::numeric_limits<int64_t>::min()) {
        pendingFirstPts_ = ptsUs;
    }
}

void ClipIndex::updateRetainPosition() noexcept {
    if (!enabled_.load(std::memory_order_relaxed) || !ring_) return;
    std::lock_guard<std::mutex> lk(mutex_);
    if (clusterCount_ == 0) return;

    // Find the oldest valid cluster
    size_t count = std::min(clusterCount_, config::clip::kMaxClusters);
    for (size_t i = count; i > 0; --i) {
        size_t idx = (clusterWriteIdx_ + config::clip::kMaxClusters - i) % config::clip::kMaxClusters;
        const auto& c = clusters_[idx];
        long long localPos = c.absOffset - ring_->baseOffset();
        if (localPos >= 0) {
            ring_->setRetainPos(static_cast<size_t>(localPos));
            return;
        }
    }
}

ClipIndex::ClipMetrics ClipIndex::clipMetrics() const noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    ClipMetrics m;
    m.enabled = enabled_.load(std::memory_order_relaxed);
    m.bufferCapacity = ring_ ? ring_->capacity() : 0;
    m.bytesUsed = ring_ ? ring_->retainedBytes() : 0;
    m.clusterCount = std::min(clusterCount_, config::clip::kMaxClusters);
    m.availableSeconds = availableRangeSecondsLocked();
    return m;
}

ClipIndex::ClipResult ClipIndex::extractClip(float lastNSeconds) noexcept {
    std::lock_guard<std::mutex> lk(mutex_);

    if (!ring_) return {{}, "no ring buffer available"};
    if (streamHeader_.empty()) return {{}, "no stream header available"};

    // Seal the last pending cluster so it's included in extraction
    long long endPos = ring_->baseOffset() +
        static_cast<long long>(ring_->currentWritePos());
    sealPendingCluster(endPos);

    if (clusterCount_ == 0) return {{}, "no clusters buffered"};

    // Find the newest cluster's PTS to compute cutoff
    size_t newestIdx = (clusterWriteIdx_ + config::clip::kMaxClusters - 1) % config::clip::kMaxClusters;
    int64_t newestPts = clusters_[newestIdx].firstPtsUs;
    int64_t cutoffPts = newestPts - static_cast<int64_t>(lastNSeconds * 1'000'000.0);

    // Walk backward through clusters to find the first keyframe cluster at or before cutoff
    size_t count = std::min(clusterCount_, config::clip::kMaxClusters);
    int clipStartIdx = -1;

    for (size_t i = 0; i < count; ++i) {
        size_t idx = (clusterWriteIdx_ + config::clip::kMaxClusters - 1 - i) % config::clip::kMaxClusters;
        const auto& c = clusters_[idx];
        if (!isClusterAvailable(c)) break;
        if (c.hasKeyFrame && c.firstPtsUs <= cutoffPts) {
            clipStartIdx = static_cast<int>(idx);
            break;
        }
    }

    // If no keyframe found before cutoff, use the earliest keyframe available
    if (clipStartIdx < 0) {
        for (size_t i = count; i > 0; --i) {
            size_t idx = (clusterWriteIdx_ + config::clip::kMaxClusters - i) % config::clip::kMaxClusters;
            const auto& c = clusters_[idx];
            if (!isClusterAvailable(c)) continue;
            if (c.hasKeyFrame) {
                clipStartIdx = static_cast<int>(idx);
                break;
            }
        }
    }

    if (clipStartIdx < 0) return {{}, "no keyframe found in buffer"};

    return assembleClip(static_cast<size_t>(clipStartIdx), newestIdx, count);
}

ClipIndex::ClipResult ClipIndex::extractFromPts(int64_t targetPtsUs) noexcept {
    std::lock_guard<std::mutex> lk(mutex_);

    if (!ring_) return {{}, "no ring buffer available"};
    if (streamHeader_.empty()) return {{}, "no stream header available"};

    long long endPos = ring_->baseOffset() +
        static_cast<long long>(ring_->currentWritePos());
    sealPendingCluster(endPos);

    if (clusterCount_ == 0) return {{}, "no clusters buffered"};

    size_t count = std::min(clusterCount_, config::clip::kMaxClusters);
    size_t newestIdx = (clusterWriteIdx_ + config::clip::kMaxClusters - 1) % config::clip::kMaxClusters;

    // Find nearest keyframe cluster at or before targetPtsUs
    int startIdx = -1;
    for (size_t i = 0; i < count; ++i) {
        size_t idx = (clusterWriteIdx_ + config::clip::kMaxClusters - 1 - i) % config::clip::kMaxClusters;
        const auto& c = clusters_[idx];
        if (!isClusterAvailable(c)) break;
        if (c.hasKeyFrame && c.firstPtsUs <= targetPtsUs) {
            startIdx = static_cast<int>(idx);
            break;
        }
    }
    if (startIdx < 0) return {{}, "no keyframe found at target position"};

    return assembleClip(static_cast<size_t>(startIdx), newestIdx, count);
}

float ClipIndex::availableRangeSeconds() const noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    return availableRangeSecondsLocked();
}

void ClipIndex::reset() noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    clusterCount_ = 0;
    clusterWriteIdx_ = 0;
    hasPendingCluster_ = false;
    streamHeader_.clear();
    if (ring_) ring_->setRetainPos(0);
}

std::string ClipIndex::generateFilePath() noexcept {
    std::string dir;
    { std::lock_guard<std::mutex> lk(tempDirMtx_); dir = tempDir_; }
    return dir + "/clip_" + std::to_string(nowMs()) + ".webm";
}

void ClipIndex::setTempDir(const std::string& dir) noexcept {
    std::lock_guard<std::mutex> lk(tempDirMtx_);
    tempDir_ = dir;
}

bool ClipIndex::isClusterAvailable(const ClusterRecord& c) const noexcept {
    long long localPos = c.absOffset - ring_->baseOffset();
    if (localPos < 0) return false;
    return static_cast<size_t>(localPos) >= ring_->compactPos();
}

ClipIndex::ClipResult ClipIndex::assembleClip(size_t startIdx, size_t newestIdx,
                                               size_t count) noexcept {
    // Freeze compaction at the oldest cluster we need, preventing the
    // ingest thread from compacting past our read window.
    const auto& oldest = clusters_[startIdx];
    long long localPos = oldest.absOffset - ring_->baseOffset();
    size_t savedRetain = ring_->retainPos();
    if (localPos >= 0) {
        ring_->setRetainPos(static_cast<size_t>(localPos));
    }

    size_t clipDataSize = 0;
    std::vector<size_t> clusterIndices;
    for (size_t idx = startIdx; ; ) {
        const auto& c = clusters_[idx];
        if (!isClusterAvailable(c)) {
            ring_->setRetainPos(savedRetain);
            return {{}, "clip data compacted during extraction"};
        }
        clipDataSize += c.size;
        clusterIndices.push_back(idx);
        if (idx == newestIdx) break;
        idx = (idx + 1) % config::clip::kMaxClusters;
        if (clusterIndices.size() > count) break;
    }

    // Defense-in-depth: bound the final allocation even if cluster sizes
    // sum into something absurd through a malformed stream. 64 MiB is
    // ~4 minutes at 2 Mbps — well beyond any realistic DVR window and
    // tight enough to keep a single clip extraction from spiking memory
    // pressure on low-end Android devices.
    constexpr size_t kMaxClipBytes = 64ull * 1024ull * 1024ull;
    if (streamHeader_.size() > kMaxClipBytes ||
        clipDataSize > kMaxClipBytes - streamHeader_.size()) {
        ring_->setRetainPos(savedRetain);
        return {{}, "clip size exceeds hard cap"};
    }

    std::vector<uint8_t> output(streamHeader_.size() + clipDataSize);
    std::memcpy(output.data(), streamHeader_.data(), streamHeader_.size());

    size_t outPos = streamHeader_.size();
    for (size_t idx : clusterIndices) {
        const auto& c = clusters_[idx];
        ReadRegion region = ring_->readRegion(c.absOffset, c.size);
        if (region.total() != c.size) {
            ring_->setRetainPos(savedRetain);
            return {{}, "clip data no longer in ring"};
        }
        std::memcpy(output.data() + outPos, region.data1, region.len1);
        if (region.len2 > 0) {
            std::memcpy(output.data() + outPos + region.len1, region.data2, region.len2);
        }
        outPos += c.size;
    }

    ring_->setRetainPos(savedRetain);
    return {std::move(output), {}};
}

float ClipIndex::availableRangeSecondsLocked() const noexcept {
    if (clusterCount_ < 2) return 0.0f;

    size_t count = std::min(clusterCount_, config::clip::kMaxClusters);
    size_t newestIdx = (clusterWriteIdx_ + config::clip::kMaxClusters - 1) % config::clip::kMaxClusters;
    int64_t newestPts = clusters_[newestIdx].firstPtsUs;

    int64_t oldestPts = newestPts;
    for (size_t i = count; i > 0; --i) {
        size_t idx = (clusterWriteIdx_ + config::clip::kMaxClusters - i) % config::clip::kMaxClusters;
        const auto& c = clusters_[idx];
        if (!ring_ || !isClusterAvailable(c)) continue;
        oldestPts = c.firstPtsUs;
        break;
    }
    return static_cast<float>(newestPts - oldestPts) / 1'000'000.0f;
}

void ClipIndex::sealPendingCluster(long long nextAbsOffset) noexcept {
    if (!hasPendingCluster_) return;
    if (nextAbsOffset <= pendingClusterAbsOffset_) return;
    // A cluster without any block PTS (e.g. aborted mid-parse, pure-Cues) leaves
    // pendingFirstPts_ at INT64_MIN. Recording that sentinel would poison downstream
    // signed-arithmetic (extractClip cutoff, availableRangeSeconds). Drop instead.
    if (pendingFirstPts_ == std::numeric_limits<int64_t>::min()) {
        hasPendingCluster_ = false;
        return;
    }
    auto size = static_cast<size_t>(nextAbsOffset - pendingClusterAbsOffset_);
    // Inner sanity bound: a single libwebm-supplied cluster span larger than
    // the whole-clip cap is malformed — drop the pending record rather than
    // store a poisonous size that taints availableRangeSeconds and the clip
    // ring. The outer assembleClip cap catches the cumulative case; this one
    // keeps a single bad cluster from corrupting the index ring for callers
    // that never extract a clip.
    constexpr size_t kMaxSinglePendingClusterBytes = 64ull * 1024ull * 1024ull;
    if (size > kMaxSinglePendingClusterBytes) {
        hasPendingCluster_ = false;
        return;
    }

    auto& rec = clusters_[clusterWriteIdx_];
    rec.absOffset = pendingClusterAbsOffset_;
    rec.size = size;
    rec.firstPtsUs = pendingFirstPts_;
    rec.hasKeyFrame = pendingHasKeyFrame_;

    clusterWriteIdx_ = (clusterWriteIdx_ + 1) % config::clip::kMaxClusters;
    if (clusterCount_ < config::clip::kMaxClusters) ++clusterCount_;
    hasPendingCluster_ = false;
}

int64_t ClipIndex::nowMs() noexcept {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
}

}  // namespace media
