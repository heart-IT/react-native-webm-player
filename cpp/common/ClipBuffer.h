// Rolling ring buffer of raw WebM bytes for zero-reencode clip extraction.
//
// feedData() tees raw bytes here. On capture, walk backward to nearest
// keyframe, prepend cached EBML+Segment+Tracks header, emit standalone WebM.
//
// Threading: append/onNewCluster/onKeyFrame called from JS thread only.
// extractClip called from background thread (mutex-protected, not RT).
#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <mutex>
#include <algorithm>
#include <cstring>
#include "MediaConfig.h"
#include "MediaLog.h"
#include "Platform.h"

namespace media {

namespace config::clip {
    constexpr size_t kDefaultBufferSize = 4 * 1024 * 1024;   // 4MB
    constexpr float kMaxDurationSeconds = 120.0f;
    constexpr size_t kMaxClusters = 512;
}

class ClipBuffer {
public:
    ClipBuffer() noexcept {
        clusters_.reserve(config::clip::kMaxClusters);
    }

    ~ClipBuffer() = default;

    ClipBuffer(const ClipBuffer&) = delete;
    ClipBuffer& operator=(const ClipBuffer&) = delete;

    void setEnabled(bool enabled) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        bool was = enabled_.load(std::memory_order_relaxed);
        if (enabled && !was) {
            ringBuffer_.resize(bufferSize_);
            writePos_ = 0;
            totalBytesWritten_ = 0;
            clusterCount_ = 0;
            clusterWriteIdx_ = 0;
            clusters_.resize(config::clip::kMaxClusters);
            pendingClusterSize_ = 0;
            hasPendingCluster_ = false;
        } else if (!enabled && was) {
            ringBuffer_.clear();
            ringBuffer_.shrink_to_fit();
            clusters_.clear();
        }
        enabled_.store(enabled, std::memory_order_release);
    }

    [[nodiscard]] bool isEnabled() const noexcept { return enabled_.load(std::memory_order_acquire); }

    void setBufferSize(size_t bytes) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        bufferSize_ = bytes;
    }

    void setStreamHeader(const uint8_t* data, size_t len) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        streamHeader_.assign(data, data + len);
    }

    [[nodiscard]] bool hasStreamHeader() const noexcept {
        return !streamHeader_.empty();
    }

    // Append raw WebM bytes to the ring buffer (called from feedData).
    void appendBytes(const uint8_t* data, size_t len) noexcept {
        if (!enabled_.load(std::memory_order_relaxed) || len == 0) return;
        std::lock_guard<std::mutex> lk(mutex_);

        if (ringBuffer_.empty()) return;
        size_t cap = ringBuffer_.size();

        // Write bytes into circular buffer
        size_t firstChunk = std::min(len, cap - writePos_);
        std::memcpy(ringBuffer_.data() + writePos_, data, firstChunk);
        if (firstChunk < len) {
            std::memcpy(ringBuffer_.data(), data + firstChunk, len - firstChunk);
        }
        writePos_ = (writePos_ + len) % cap;
        totalBytesWritten_ += len;
        pendingClusterSize_ += len;
    }

    // Called when the demuxer enters a new cluster. Seals the previous
    // pending cluster (if any) into a ClusterRecord.
    void onNewCluster() noexcept {
        if (!enabled_.load(std::memory_order_relaxed)) return;
        std::lock_guard<std::mutex> lk(mutex_);
        sealPendingCluster();
        hasPendingCluster_ = true;
        pendingClusterStart_ = writePos_;
        pendingClusterByteCounter_ = totalBytesWritten_;
        pendingClusterSize_ = 0;
        pendingHasKeyFrame_ = false;
        pendingFirstPts_ = INT64_MIN;
    }

    void onKeyFrame(int64_t ptsUs) noexcept {
        if (!enabled_.load(std::memory_order_relaxed)) return;
        std::lock_guard<std::mutex> lk(mutex_);
        pendingHasKeyFrame_ = true;
        if (pendingFirstPts_ == INT64_MIN) {
            pendingFirstPts_ = ptsUs;
        }
    }

    void onBlockPts(int64_t ptsUs) noexcept {
        if (!enabled_.load(std::memory_order_relaxed)) return;
        std::lock_guard<std::mutex> lk(mutex_);
        if (pendingFirstPts_ == INT64_MIN) {
            pendingFirstPts_ = ptsUs;
        }
    }

    struct ClipResult {
        std::vector<uint8_t> data;
        std::string error;
    };

    [[nodiscard]] ClipResult extractClip(float lastNSeconds) const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);

        if (streamHeader_.empty()) {
            return {{}, "no stream header available"};
        }
        if (clusterCount_ == 0) {
            return {{}, "no clusters buffered"};
        }

        // Find the newest cluster's PTS to compute cutoff
        size_t newestIdx = (clusterWriteIdx_ + config::clip::kMaxClusters - 1) % config::clip::kMaxClusters;
        int64_t newestPts = clusters_[newestIdx].firstPtsUs;
        int64_t cutoffPts = newestPts - static_cast<int64_t>(lastNSeconds * 1'000'000.0f);

        // Walk backward through clusters to find the first keyframe cluster at or before cutoff
        size_t count = std::min(clusterCount_, config::clip::kMaxClusters);
        int clipStartIdx = -1;

        for (size_t i = 0; i < count; ++i) {
            size_t idx = (clusterWriteIdx_ + config::clip::kMaxClusters - 1 - i) % config::clip::kMaxClusters;
            const auto& c = clusters_[idx];

            // Check if this cluster's data has been overwritten
            if (totalBytesWritten_ - c.byteCounter > ringBuffer_.size()) {
                break;
            }

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
                if (totalBytesWritten_ - c.byteCounter > ringBuffer_.size()) continue;
                if (c.hasKeyFrame) {
                    clipStartIdx = static_cast<int>(idx);
                    break;
                }
            }
        }

        if (clipStartIdx < 0) {
            return {{}, "no keyframe found in buffer"};
        }

        // Calculate total clip byte size
        size_t clipDataSize = 0;
        std::vector<size_t> clusterIndices;
        for (size_t idx = static_cast<size_t>(clipStartIdx); ; ) {
            const auto& c = clusters_[idx];
            if (totalBytesWritten_ - c.byteCounter > ringBuffer_.size()) {
                return {{}, "clip data overwritten during extraction"};
            }
            clipDataSize += c.size;
            clusterIndices.push_back(idx);
            if (idx == newestIdx) break;
            idx = (idx + 1) % config::clip::kMaxClusters;
            if (clusterIndices.size() > count) break;
        }

        // Assemble output: header + cluster bytes
        std::vector<uint8_t> output;
        output.resize(streamHeader_.size() + clipDataSize);
        std::memcpy(output.data(), streamHeader_.data(), streamHeader_.size());

        size_t outPos = streamHeader_.size();
        size_t cap = ringBuffer_.size();
        for (size_t idx : clusterIndices) {
            const auto& c = clusters_[idx];
            copyFromRing(output.data() + outPos, c.ringOffset, c.size, cap);
            outPos += c.size;
        }

        return {std::move(output), {}};
    }

    // Extract bytes from an absolute PTS target through the newest cluster.
    // Used by seekTo() to re-feed buffered data through the demuxer.
    [[nodiscard]] ClipResult extractFromPts(int64_t targetPtsUs) const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);

        if (streamHeader_.empty()) return {{}, "no stream header available"};
        if (clusterCount_ == 0) return {{}, "no clusters buffered"};

        size_t count = std::min(clusterCount_, config::clip::kMaxClusters);
        size_t newestIdx = (clusterWriteIdx_ + config::clip::kMaxClusters - 1) % config::clip::kMaxClusters;

        // Find nearest keyframe cluster at or before targetPtsUs
        int startIdx = -1;
        for (size_t i = 0; i < count; ++i) {
            size_t idx = (clusterWriteIdx_ + config::clip::kMaxClusters - 1 - i) % config::clip::kMaxClusters;
            const auto& c = clusters_[idx];
            if (totalBytesWritten_ - c.byteCounter > ringBuffer_.size()) break;
            if (c.hasKeyFrame && c.firstPtsUs <= targetPtsUs) {
                startIdx = static_cast<int>(idx);
                break;
            }
        }
        if (startIdx < 0) return {{}, "no keyframe found at target position"};

        size_t clipDataSize = 0;
        std::vector<size_t> indices;
        for (size_t idx = static_cast<size_t>(startIdx); ; ) {
            const auto& c = clusters_[idx];
            if (totalBytesWritten_ - c.byteCounter > ringBuffer_.size()) {
                return {{}, "data overwritten during extraction"};
            }
            clipDataSize += c.size;
            indices.push_back(idx);
            if (idx == newestIdx) break;
            idx = (idx + 1) % config::clip::kMaxClusters;
            if (indices.size() > count) break;
        }

        std::vector<uint8_t> output(streamHeader_.size() + clipDataSize);
        std::memcpy(output.data(), streamHeader_.data(), streamHeader_.size());
        size_t outPos = streamHeader_.size();
        size_t cap = ringBuffer_.size();
        for (size_t idx : indices) {
            const auto& c = clusters_[idx];
            copyFromRing(output.data() + outPos, c.ringOffset, c.size, cap);
            outPos += c.size;
        }
        return {std::move(output), {}};
    }

    // How far back (in seconds) the buffer extends from the newest data.
    [[nodiscard]] float availableRangeSeconds() const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        if (clusterCount_ < 2) return 0.0f;

        size_t count = std::min(clusterCount_, config::clip::kMaxClusters);
        size_t newestIdx = (clusterWriteIdx_ + config::clip::kMaxClusters - 1) % config::clip::kMaxClusters;
        int64_t newestPts = clusters_[newestIdx].firstPtsUs;

        // Walk backward to find oldest valid cluster
        int64_t oldestPts = newestPts;
        for (size_t i = count; i > 0; --i) {
            size_t idx = (clusterWriteIdx_ + config::clip::kMaxClusters - i) % config::clip::kMaxClusters;
            const auto& c = clusters_[idx];
            if (totalBytesWritten_ - c.byteCounter > ringBuffer_.size()) continue;
            oldestPts = c.firstPtsUs;
            break;
        }
        return static_cast<float>(newestPts - oldestPts) / 1'000'000.0f;
    }

    void reset() noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        writePos_ = 0;
        totalBytesWritten_ = 0;
        clusterCount_ = 0;
        clusterWriteIdx_ = 0;
        pendingClusterSize_ = 0;
        hasPendingCluster_ = false;
        streamHeader_.clear();
    }

    static std::string generateFilePath() noexcept {
#if defined(__ANDROID__)
        // Android: use /data/data/<pkg>/cache/ — caller should set via setTempDir
        return tempDir_ + "/clip_" + std::to_string(nowMs()) + ".webm";
#else
        // iOS: NSTemporaryDirectory handled in .mm wrapper
        return tempDir_ + "/clip_" + std::to_string(nowMs()) + ".webm";
#endif
    }

    static void setTempDir(const std::string& dir) noexcept {
        tempDir_ = dir;
    }

private:
    struct ClusterRecord {
        size_t ringOffset = 0;
        size_t size = 0;
        int64_t firstPtsUs = 0;
        bool hasKeyFrame = false;
        uint64_t byteCounter = 0;
    };

    void sealPendingCluster() noexcept {
        if (!hasPendingCluster_ || pendingClusterSize_ == 0) return;

        auto& rec = clusters_[clusterWriteIdx_];
        rec.ringOffset = pendingClusterStart_;
        rec.size = pendingClusterSize_;
        rec.firstPtsUs = pendingFirstPts_;
        rec.hasKeyFrame = pendingHasKeyFrame_;
        rec.byteCounter = pendingClusterByteCounter_;

        clusterWriteIdx_ = (clusterWriteIdx_ + 1) % config::clip::kMaxClusters;
        if (clusterCount_ < config::clip::kMaxClusters) ++clusterCount_;
    }

    void copyFromRing(uint8_t* dst, size_t ringOffset, size_t len, size_t cap) const noexcept {
        size_t firstChunk = std::min(len, cap - ringOffset);
        std::memcpy(dst, ringBuffer_.data() + ringOffset, firstChunk);
        if (firstChunk < len) {
            std::memcpy(dst + firstChunk, ringBuffer_.data(), len - firstChunk);
        }
    }

    static int64_t nowMs() noexcept {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
    }

    mutable std::mutex mutex_;
    std::atomic<bool> enabled_{false};
    size_t bufferSize_ = config::clip::kDefaultBufferSize;

    std::vector<uint8_t> ringBuffer_;
    size_t writePos_ = 0;
    uint64_t totalBytesWritten_ = 0;

    std::vector<ClusterRecord> clusters_;
    size_t clusterWriteIdx_ = 0;
    size_t clusterCount_ = 0;

    // Pending cluster accumulation
    size_t pendingClusterStart_ = 0;
    uint64_t pendingClusterByteCounter_ = 0;
    size_t pendingClusterSize_ = 0;
    bool pendingHasKeyFrame_ = false;
    int64_t pendingFirstPts_ = INT64_MIN;
    bool hasPendingCluster_ = false;

    std::vector<uint8_t> streamHeader_;

    static inline std::string tempDir_ = "/tmp";
};

}  // namespace media
