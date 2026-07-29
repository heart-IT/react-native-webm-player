// Parses the channel layout out of an OpusHead (RFC 7845 §5.1).
//
// Anything above stereo is carried as several Opus streams plus a mapping table,
// and must be decoded with the multistream API — `opus_decoder_create` rejects
// channels > 2 with OPUS_BAD_ARG. A 5.1 stream therefore produces no audio at
// all unless the mapping is read and honoured.
#pragma once

#include <cstddef>
#include <cstdint>

namespace media {

struct OpusChannelMapping {
    int channels = 0;
    int streams = 0;
    int coupledStreams = 0;
    int mappingFamily = 0;
    /// Index per output channel. Only meaningful for family != 0.
    uint8_t mapping[255] = {};
    bool valid = false;

    /// True when the multistream decoder is required.
    bool needsMultistream() const noexcept {
        return mappingFamily != 0 || channels > 2;
    }
};

/// Returns an invalid mapping if `head` is not a usable OpusHead. Callers should
/// fall back to a plain stereo/mono decoder in that case.
inline OpusChannelMapping parseOpusHead(const uint8_t* head, size_t size) noexcept {
    OpusChannelMapping out;
    // "OpusHead" + version + channels + preskip + rate + gain + family = 19
    if (!head || size < 19) return out;
    if (head[0] != 'O' || head[1] != 'p' || head[2] != 'u' || head[3] != 's' ||
        head[4] != 'H' || head[5] != 'e' || head[6] != 'a' || head[7] != 'd') {
        return out;
    }

    out.channels = head[9];
    out.mappingFamily = head[18];
    if (out.channels <= 0) return out;

    if (out.mappingFamily == 0) {
        // Family 0 is mono or stereo in one stream, with no mapping table.
        if (out.channels > 2) return out;
        out.streams = 1;
        out.coupledStreams = out.channels - 1;
        for (int i = 0; i < out.channels; i++) out.mapping[i] = static_cast<uint8_t>(i);
        out.valid = true;
        return out;
    }

    // Families 1..255 carry stream counts and an explicit per-channel table.
    const size_t needed = 21 + static_cast<size_t>(out.channels);
    if (size < needed) return out;

    out.streams = head[19];
    out.coupledStreams = head[20];
    if (out.streams <= 0 || out.coupledStreams < 0 ||
        out.coupledStreams > out.streams ||
        out.streams + out.coupledStreams > 255) {
        return out;
    }
    for (int i = 0; i < out.channels; i++) {
        out.mapping[i] = head[21 + i];
    }
    out.valid = true;
    return out;
}

}  // namespace media
