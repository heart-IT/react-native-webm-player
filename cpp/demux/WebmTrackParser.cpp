// parseTracks() — walks the segment's track list, fills TrackInfo for the
// supported codecs (A_OPUS + V_VP9), and captures the WebM stream header
// bytes (EBML + Segment + Tracks) for clip extraction.
//
// Member of WebmDemuxer; class definition lives in WebmDemuxer.h.
#include "WebmDemuxer.h"
#include "DemuxLimits.h"

#include "common/MediaTime.h"

#include "mkvparser/mkvparser.h"

#include <cstring>
#include <mutex>

namespace media::demux {

bool WebmDemuxer::parseTracks() {
    if (!segment_) {
        return false;
    }

    // Load headers (tracks, seek head, info, etc.)
    // For streaming WebM (unknown segment size), Load() may return
    // E_BUFFER_NOT_FULL even after tracks and clusters are parsed.
    // Check if tracks are available regardless of return value.
    long ret = segment_->Load();

    const mkvparser::Tracks* tracks = segment_->GetTracks();
    if (!tracks || tracks->GetTracksCount() == 0) {
        if (ret < 0) {
            if (ret == mkvparser::E_BUFFER_NOT_FULL) {
                return false;  // Need more data
            }
            parseError_ = "segment load failed";
            parseErrorCount_.fetch_add(1, std::memory_order_relaxed);
            cumulativeParseErrorCount_.fetch_add(1, std::memory_order_relaxed);
            if (++parseRetryCount_ >= kMaxParseRetries) {
                state_ = ParseState::Error;
                errorEntryUs_.store(nowUs(), std::memory_order_relaxed);
            }
        }
        return false;
    }

    unsigned long trackCount = tracks->GetTracksCount();
    if (trackCount > kMaxTracks) trackCount = kMaxTracks;
    std::lock_guard<std::mutex> trackLk(trackMtx_);
    for (unsigned long i = 0; i < trackCount; ++i) {
        const mkvparser::Track* track = tracks->GetTrackByIndex(i);
        if (!track) {
            continue;
        }

        long long trackType = track->GetType();
        const char* codecId = track->GetCodecId();
        if (!codecId) {
            continue;
        }

        size_t codecPrivateSize = 0;
        const unsigned char* codecPrivate = track->GetCodecPrivate(codecPrivateSize);

        if (trackType == mkvparser::Track::kAudio &&
            codecId && std::strcmp(codecId, "A_OPUS") == 0) {
            trackInfo_.audioTrackNum = static_cast<int>(track->GetNumber());
            trackInfo_.audioCodecId = codecId;
            if (codecPrivate && codecPrivateSize > 0) {
                trackInfo_.audioCodecPrivate.assign(
                    codecPrivate, codecPrivate + codecPrivateSize);
            }
        } else if (trackType == mkvparser::Track::kVideo &&
                   codecId && std::strcmp(codecId, "V_VP9") == 0) {
            trackInfo_.videoTrackNum = static_cast<int>(track->GetNumber());
            trackInfo_.videoCodecId = codecId;
            if (codecPrivate && codecPrivateSize > 0) {
                trackInfo_.videoCodecPrivate.assign(
                    codecPrivate, codecPrivate + codecPrivateSize);
            }
            const mkvparser::VideoTrack* videoTrack =
                static_cast<const mkvparser::VideoTrack*>(track);
            trackInfo_.videoWidth = static_cast<int>(videoTrack->GetWidth());
            trackInfo_.videoHeight = static_cast<int>(videoTrack->GetHeight());
        }
    }

    if (trackInfo_.audioTrackNum < 0 && trackInfo_.videoTrackNum < 0) {
        parseError_ = "no supported tracks (need A_OPUS or V_VP9)";
        parseErrorCount_.fetch_add(1, std::memory_order_relaxed);
        cumulativeParseErrorCount_.fetch_add(1, std::memory_order_relaxed);
        state_ = ParseState::Error;
        errorEntryUs_.store(nowUs(), std::memory_order_relaxed);
        return false;
    }

    // Initialize cluster iteration
    cluster_ = segment_->GetFirst();
    blockEntry_ = nullptr;

    // Capture stream header bytes (EBML + Segment + Tracks) for clip extraction.
    // Everything from byte 0 to the first cluster position forms a valid WebM
    // header that can be prepended to cluster data for standalone playback.
    if (cluster_ && !cluster_->EOS()) {
        long long headerEnd = cluster_->GetPosition();
        if (headerEnd > 0 && static_cast<size_t>(headerEnd) <= kMaxStreamHeaderSize
            && streamHeader_.empty()) {
            streamHeader_.resize(static_cast<size_t>(headerEnd));
            int readResult = activeReader()->Read(0, static_cast<long>(headerEnd),
                                           streamHeader_.data());
            if (readResult != 0) {
                streamHeader_.clear();
            }
        }
    }

    return true;
}

}  // namespace media::demux
