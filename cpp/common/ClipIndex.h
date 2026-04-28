// Metadata-only index into IngestRingBuffer for zero-reencode clip extraction.
//
// Tracks cluster boundaries, keyframe positions, and PTS ranges. On capture,
// walks backward to nearest keyframe, reads raw bytes directly from the
// IngestRingBuffer (no separate data copy), prepends cached EBML+Segment+Tracks
// header, and emits a standalone WebM file.
//
// Retention: updates IngestRingBuffer::setRetainPos() to prevent compaction
// past the oldest tracked cluster, keeping historical data readable.
//
// Threading: onNewCluster/onKeyFrame/onBlockPts called from ingest thread only.
// extractClip/extractFromPts called from background thread (mutex-protected).
#pragma once

#include <atomic>
#include <climits>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace media {

class IngestRingBuffer;

namespace config::clip {
    constexpr float kMaxDurationSeconds = 120.0f;
    constexpr size_t kMaxClusters = 512;
}

class ClipIndex {
public:
    ClipIndex() noexcept;
    ~ClipIndex() = default;

    ClipIndex(const ClipIndex&) = delete;
    ClipIndex& operator=(const ClipIndex&) = delete;

    void setRingBuffer(IngestRingBuffer* ring) noexcept;
    void setEnabled(bool enabled) noexcept;

    [[nodiscard]] bool isEnabled() const noexcept { return enabled_.load(std::memory_order_acquire); }

    void setStreamHeader(const uint8_t* data, size_t len) noexcept;
    [[nodiscard]] bool hasStreamHeader() const noexcept;
    [[nodiscard]] std::vector<uint8_t> streamHeader() const noexcept;

    // Called when the demuxer enters a new cluster. Seals the previous
    // pending cluster (computing its size from position difference) and
    // records the new cluster's absolute byte position.
    void onNewCluster(long long absOffset) noexcept;
    void onKeyFrame(int64_t ptsUs) noexcept;
    void onBlockPts(int64_t ptsUs) noexcept;

    // Update the ring buffer's retain position to protect the oldest tracked cluster.
    void updateRetainPosition() noexcept;

    struct ClipMetrics {
        bool enabled = false;
        size_t bufferCapacity = 0;
        size_t bytesUsed = 0;
        size_t clusterCount = 0;
        float availableSeconds = 0.0f;
    };

    [[nodiscard]] ClipMetrics clipMetrics() const noexcept;

    struct ClipResult {
        std::vector<uint8_t> data;
        std::string error;
    };

    [[nodiscard]] ClipResult extractClip(float lastNSeconds) noexcept;

    // Extract bytes from an absolute PTS target through the newest cluster.
    // Used by seekTo() to re-feed buffered data through the demuxer.
    [[nodiscard]] ClipResult extractFromPts(int64_t targetPtsUs) noexcept;

    // How far back (in seconds) the buffer extends from the newest data.
    [[nodiscard]] float availableRangeSeconds() const noexcept;

    void reset() noexcept;

    static std::string generateFilePath() noexcept;
    static void setTempDir(const std::string& dir) noexcept;

private:
    struct ClusterRecord {
        long long absOffset = 0;
        size_t size = 0;
        int64_t firstPtsUs = 0;
        bool hasKeyFrame = false;
    };

    [[nodiscard]] bool isClusterAvailable(const ClusterRecord& c) const noexcept;
    [[nodiscard]] ClipResult assembleClip(size_t startIdx, size_t newestIdx, size_t count) noexcept;
    // Caller must hold mutex_.
    [[nodiscard]] float availableRangeSecondsLocked() const noexcept;
    void sealPendingCluster(long long nextAbsOffset) noexcept;
    static int64_t nowMs() noexcept;

    mutable std::mutex mutex_;
    std::atomic<bool> enabled_{false};
    IngestRingBuffer* ring_{nullptr};

    std::vector<ClusterRecord> clusters_;
    size_t clusterWriteIdx_ = 0;
    size_t clusterCount_ = 0;

    // Pending cluster accumulation
    long long pendingClusterAbsOffset_ = 0;
    bool pendingHasKeyFrame_ = false;
    int64_t pendingFirstPts_ = INT64_MIN;
    bool hasPendingCluster_ = false;

    std::vector<uint8_t> streamHeader_;

    static inline std::mutex tempDirMtx_;
    static inline std::string tempDir_ = "/tmp";
};

}  // namespace media
