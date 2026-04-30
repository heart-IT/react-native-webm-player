#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <vector>
#include <string>
#include <memory>
#include "common/MediaTime.h"

// Forward declarations for libwebm types
namespace mkvparser {
class IMkvReader;
class EBMLHeader;
class Segment;
class Cluster;
class BlockEntry;
}  // namespace mkvparser

namespace media { class IngestRingBuffer; }

namespace media::demux {

// Opus audio packet extracted from WebM. Points into demuxer's internal buffer —
// valid only until the next feedData() or reset() call.
struct AudioPacket {
    const uint8_t* data;      // Raw Opus packet bytes (into ring or scratch buffer)
    size_t size;              // Packet size in bytes (max 1275 per Opus spec)
    int64_t ptsUs;            // Presentation timestamp in microseconds
    int64_t durationUs;       // Packet duration in microseconds (typically 20000)
    long long absOffset = 0;  // Absolute byte position in IngestRingBuffer
};

// VP9 video packet extracted from WebM. Same lifetime rules as AudioPacket.
struct VideoPacket {
    const uint8_t* data;      // Raw VP9 frame bytes (into ring or scratch buffer)
    size_t size;              // Frame size in bytes (up to ~512KB for 4K keyframes)
    int64_t ptsUs;            // Presentation timestamp in microseconds
    bool isKeyFrame;          // True for keyframes (decoder can start from here)
    long long absOffset = 0;  // Absolute byte position in IngestRingBuffer
};

// Packets in DemuxResult point into an internal buffer owned by WebmDemuxer.
// They remain valid until the next call to feedData() or reset().
// Callers must consume or copy packet data before calling feedData() again.
struct DemuxResult {
    std::vector<AudioPacket> audioPackets;  // Opus packets extracted this call
    std::vector<VideoPacket> videoPackets;  // VP9 packets extracted this call
    std::string error;                      // Non-empty on parse error (logged, not fatal)
    int newClusterCount = 0;                // New WebM clusters encountered this call
    std::vector<long long> newClusterPositions;  // Absolute byte positions of new clusters
};

struct TrackInfo {
    int audioTrackNum = -1;
    int videoTrackNum = -1;
    int videoWidth = 0;
    int videoHeight = 0;
    std::string audioCodecId;
    std::string videoCodecId;
    std::vector<uint8_t> audioCodecPrivate;  // OpusHead
    std::vector<uint8_t> videoCodecPrivate;  // VP9 CodecPrivate
};

enum class ParseState {
    WaitingForEBML,
    WaitingForSegment,
    ParsingTracks,
    Streaming,
    Error
};

struct DemuxerMetrics {
    uint64_t totalBytesFed = 0;
    uint64_t feedDataCalls = 0;
    uint64_t audioPacketsEmitted = 0;
    uint64_t videoPacketsEmitted = 0;
    uint64_t overflowCount = 0;
    // Aggregate drop counter (sum of the three categories below; kept for continuity).
    uint64_t partialDropCount = 0;
    // Split drop counters so triage can distinguish the cause.
    uint64_t oversizedFrameDrops = 0;      // video frame exceeded kMaxVideoFrameSize
    uint64_t packetCapDrops = 0;           // hit kMaxAudio/VideoPacketsPerFeed ceiling
    uint64_t appendBackpressureDrops = 0;  // stream-mode append lost trailing bytes
    uint64_t blockStallCount = 0;   // consecutive block parse failures triggering stall error
    uint64_t parseErrorCount = 0;   // total transient parse errors (header, segment, tracks, blocks)
    // Cumulative (monotonic across reset()) counterparts so forensic queries
    // like "any parse errors at all in this session" survive stream restarts.
    uint64_t cumulativeParseErrorCount = 0;
    uint64_t sessionResetCount = 0;
    int64_t timeInErrorMs = 0;      // milliseconds since entering ParseState::Error (0 if not in Error)
    int64_t feedJitterUs = 0;       // network-layer feed arrival jitter (EWMA)
    int64_t feedDataLatencyUs = 0;  // EWMA of feedData() parse duration
    size_t bufferBytes = 0;         // current StreamReader buffer size
    ParseState parseState = ParseState::WaitingForEBML;
    // True once parseTracks() has captured the EBML+Segment+Tracks header.
    // parseState == Error && !streamHeaderAvailable means the demuxer is wedged
    // without a cached header for self-heal — JS must call resetStream() with
    // a fresh EBML-prefixed stream.
    bool streamHeaderAvailable = false;
};

// Append-only reader for streaming WebM data into libwebm's mkvparser.
// Implements mkvparser::IMkvReader for incremental parsing.
class StreamReader;
class RingReader;

class WebmDemuxer {
public:
    WebmDemuxer();
    ~WebmDemuxer();

    WebmDemuxer(const WebmDemuxer&) = delete;
    WebmDemuxer& operator=(const WebmDemuxer&) = delete;

    // Attach a ring buffer as the backing store. When set, feedData() skips
    // the internal StreamReader copy — mkvparser reads directly from the ring.
    // Must be called before start. Pass nullptr to revert to StreamReader mode.
    void setRingBuffer(IngestRingBuffer* ring) noexcept;

    // Feed raw WebM bytes incrementally. Returns extracted packets (if any).
    // In ring mode: data/len are ignored (bytes already in ring from JS write).
    // In stream mode: data is copied into internal StreamReader.
    // Returned reference is valid until the next feedData() or reset() call.
    const DemuxResult& feedData(const uint8_t* data, size_t len);

    // Thread-safe: trackInfo is immutable after tracks are parsed.
    // Returns a copy for safe cross-thread access.
    TrackInfo trackInfoSnapshot() const noexcept {
        std::lock_guard<std::mutex> lk(trackMtx_);
        return trackInfo_;
    }

    std::vector<uint8_t> streamHeader() const noexcept {
        std::lock_guard<std::mutex> lk(trackMtx_);
        return streamHeader_;
    }
    bool hasStreamHeader() const noexcept {
        std::lock_guard<std::mutex> lk(trackMtx_);
        return !streamHeader_.empty();
    }

    ParseState parseState() const noexcept { return state_.load(std::memory_order_acquire); }
    uint64_t overflowCount() const noexcept { return overflowCount_.load(std::memory_order_relaxed); }

    size_t bufferBytes() const noexcept;

    [[nodiscard]] int64_t feedJitterUs() const noexcept { return feedJitterUs_.load(std::memory_order_relaxed); }

    DemuxerMetrics demuxerMetrics() const noexcept {
        DemuxerMetrics m{};
        m.totalBytesFed = totalBytesFed_.load(std::memory_order_relaxed);
        m.feedDataCalls = feedDataCalls_.load(std::memory_order_relaxed);
        m.audioPacketsEmitted = audioPacketsEmitted_.load(std::memory_order_relaxed);
        m.videoPacketsEmitted = videoPacketsEmitted_.load(std::memory_order_relaxed);
        m.overflowCount = overflowCount_.load(std::memory_order_relaxed);
        m.partialDropCount = partialDropCount_.load(std::memory_order_relaxed);
        m.oversizedFrameDrops = oversizedFrameDrops_.load(std::memory_order_relaxed);
        m.packetCapDrops = packetCapDrops_.load(std::memory_order_relaxed);
        m.appendBackpressureDrops = appendBackpressureDrops_.load(std::memory_order_relaxed);
        m.blockStallCount = blockStallCount_.load(std::memory_order_relaxed);
        m.parseErrorCount = parseErrorCount_.load(std::memory_order_relaxed);
        m.cumulativeParseErrorCount = cumulativeParseErrorCount_.load(std::memory_order_relaxed);
        m.sessionResetCount = sessionResetCount_.load(std::memory_order_relaxed);
        m.feedJitterUs = feedJitterUs_.load(std::memory_order_relaxed);
        m.feedDataLatencyUs = feedDataLatencyUs_.load(std::memory_order_relaxed);
        m.bufferBytes = bufferBytes();
        m.parseState = state_.load(std::memory_order_relaxed);
        if (m.parseState == ParseState::Error) {
            int64_t entry = errorEntryUs_.load(std::memory_order_relaxed);
            if (entry > 0) m.timeInErrorMs = (nowUs() - entry) / 1000;
        }
        {
            std::lock_guard<std::mutex> lk(trackMtx_);
            m.streamHeaderAvailable = !streamHeader_.empty();
        }
        return m;
    }

    void reset();

private:
    bool parseEBMLHeader();
    bool parseSegment();
    bool parseTracks();
    void parseBlocks(DemuxResult& result);

    mkvparser::IMkvReader* activeReader() const noexcept;
    size_t readerBytesAvailable() const noexcept;
    long long readerTotalAvailable() const noexcept;
    const uint8_t* readerDataAt(long long absPos, long long len) const noexcept;

    DemuxResult cachedResult_;
    std::unique_ptr<StreamReader> reader_;         // Stream mode (sync fallback)
    std::unique_ptr<RingReader> ringReader_;        // Ring mode (ingest thread)
    IngestRingBuffer* ringBuffer_{nullptr};         // Non-owning, set via setRingBuffer
    std::unique_ptr<mkvparser::EBMLHeader> ebmlHeader_;
    std::unique_ptr<mkvparser::Segment> segment_;
    const mkvparser::Cluster* cluster_ = nullptr;
    const mkvparser::BlockEntry* blockEntry_ = nullptr;
    // True when cluster_'s blocks have been fully iterated but Segment::GetNext()
    // saw no further loaded clusters yet. parseBlocks() leaves cluster_ pointing
    // at the last real cluster (rather than overwriting it with the EOS sentinel)
    // and uses this flag to drive a Load()+GetNext() retry on the next call.
    bool clusterDrained_ = false;

    mutable std::mutex trackMtx_;
    TrackInfo trackInfo_;
    std::vector<uint8_t> streamHeader_;
    std::atomic<ParseState> state_{ParseState::WaitingForEBML};
    std::string parseError_;
    long long ebmlHeaderEndPos_ = 0;
    int parseRetryCount_ = 0;

    // Track the lowest buffer offset still needed so we can compact
    long long compactOffset_ = 0;
    long long pendingCompactPos_ = 0;  // Deferred compaction target (applied at next feedData)
    std::atomic<uint64_t> overflowCount_{0};
    std::atomic<uint64_t> partialDropCount_{0};
    std::atomic<uint64_t> oversizedFrameDrops_{0};
    std::atomic<uint64_t> packetCapDrops_{0};
    std::atomic<uint64_t> appendBackpressureDrops_{0};

    // Metrics counters (atomic: written by ingest thread, read by JS via getMetrics)
    std::atomic<uint64_t> totalBytesFed_{0};
    std::atomic<uint64_t> feedDataCalls_{0};
    std::atomic<uint64_t> audioPacketsEmitted_{0};
    std::atomic<uint64_t> videoPacketsEmitted_{0};
    std::atomic<uint64_t> blockStallCount_{0};
    std::atomic<uint64_t> parseErrorCount_{0};
    // Cumulative (never zeroed by reset()) — see DemuxerMetrics.
    std::atomic<uint64_t> cumulativeParseErrorCount_{0};
    std::atomic<uint64_t> sessionResetCount_{0};
    std::atomic<int64_t> errorEntryUs_{0};  // nowUs() when state last transitioned to Error

    // PTS dedup: skip duplicate blocks from Hypercore retries
    int64_t lastEmittedAudioPtsUs_ = -1;
    int64_t lastEmittedVideoPtsUs_ = -1;

    // Feed arrival jitter tracking (network-layer signal)
    int64_t lastFeedTimeUs_ = 0;       // ingest thread only
    int64_t lastFeedIntervalUs_ = 0;   // ingest thread only
    std::atomic<int64_t> feedJitterUs_{0};
    std::atomic<int64_t> feedDataLatencyUs_{0};
    std::atomic<size_t> cachedBufferBytes_{0};

    // Per-packet scratch slots for packets that span the ring wrap boundary.
    // One slot per wrap-read so that each packet's data pointer remains stable
    // across subsequent wrap-reads in the same feedData() call. Slots are
    // reused across calls; inner buffers grow to peak usage.
    std::vector<std::vector<uint8_t>> scratchBuffers_;
    size_t scratchCursor_ = 0;  // next slot to fill within the current feedData()
};

}  // namespace media::demux
