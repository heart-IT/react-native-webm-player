// parseTracks() — walks the segment's track list and fills TrackInfo for the
// supported codecs (A_OPUS + V_VP9).
//
// Member of WebmDemuxer; class definition lives in WebmDemuxer.h.
#include "WebmDemuxer.h"
#include "DemuxLimits.h"

#include "common/MediaLog.h"

#include "mkvparser/mkvparser.h"

#include <cstring>

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
            if (++parseRetryCount_ >= kMaxParseRetries) {
                state_ = ParseState::Error;
            }
        }
        return false;
    }

    unsigned long trackCount = tracks->GetTracksCount();
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

        if (trackType == mkvparser::Track::kAudio &&
            std::strcmp(codecId, "A_OPUS") == 0) {
            trackInfo_.audioTrackNum = static_cast<int>(track->GetNumber());
            const mkvparser::AudioTrack* audioTrack =
                static_cast<const mkvparser::AudioTrack*>(track);
            // mkvparser only rejects SamplingFrequency <= 0; the double is
            // otherwise attacker-chosen, and casting it to int is UB outside
            // int's range (and for NaN, which passes the <= 0 check too).
            // Out of the sane range → 0, which the decoder configuration
            // downstream rejects cleanly (opus_decoder_create fails).
            const double samplingRate = audioTrack->GetSamplingRate();
            if (samplingRate >= 1.0 && samplingRate <= kMaxAudioSampleRate) {
                trackInfo_.audioSampleRate = static_cast<int>(samplingRate);
            } else {
                trackInfo_.audioSampleRate = 0;
                MEDIA_LOG_W("WebmDemuxer: SamplingFrequency %g out of range, "
                            "treating as invalid", samplingRate);
            }
            trackInfo_.audioChannels =
                static_cast<int>(audioTrack->GetChannels());
        } else if (trackType == mkvparser::Track::kVideo &&
                   std::strcmp(codecId, "V_VP9") == 0) {
            trackInfo_.videoTrackNum = static_cast<int>(track->GetNumber());
            const mkvparser::VideoTrack* videoTrack =
                static_cast<const mkvparser::VideoTrack*>(track);
            trackInfo_.videoWidth = static_cast<int>(videoTrack->GetWidth());
            trackInfo_.videoHeight = static_cast<int>(videoTrack->GetHeight());
            trackInfo_.videoFrameDurationNs =
                static_cast<int64_t>(track->GetDefaultDuration());
        }
    }

    if (trackInfo_.audioTrackNum < 0 && trackInfo_.videoTrackNum < 0) {
        parseError_ = "no supported tracks (need A_OPUS or V_VP9)";
        state_ = ParseState::Error;
        return false;
    }

    // Initialize cluster iteration
    cluster_ = segment_->GetFirst();
    blockEntry_ = nullptr;

    return true;
}

}  // namespace media::demux
