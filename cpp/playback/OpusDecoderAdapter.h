// Opus 1.6.1 decoder wrapper with OSCE (Deep PLC) support.
// Thread safety: NOT thread-safe. All calls must be from the decode thread.
#pragma once

#include <memory>
#include <cstring>
#include <limits>
#include <opus.h>
#include <opus_multistream.h>

#include "OpusChannelMapping.h"

namespace media {

namespace config::decoder {
// OSCE (Deep PLC) activates at decoder complexity >= 6 (LACE) or >= 7 (NoLACE).
// NoLACE provides better concealment quality than LACE at +1.1MB binary cost.
// DNN inference runs only during packet loss, not on every decoded frame.
inline constexpr int kComplexity = 7;
}  // namespace config::decoder

class OpusDecoderAdapter {
public:
    OpusDecoderAdapter() = default;
    ~OpusDecoderAdapter() noexcept { destroy(); }

    OpusDecoderAdapter(OpusDecoderAdapter&& other) noexcept
        : decoder_(other.decoder_)
        , msDecoder_(other.msDecoder_)
        , mapping_(other.mapping_)
        , sampleRate_(other.sampleRate_)
        , channels_(other.channels_)
        , lastError_(other.lastError_)
        , consecutiveErrors_(other.consecutiveErrors_) {
        other.decoder_ = nullptr;
        other.msDecoder_ = nullptr;
        other.mapping_ = OpusChannelMapping{};
        other.lastError_ = 0;
        other.consecutiveErrors_ = 0;
    }

    OpusDecoderAdapter& operator=(OpusDecoderAdapter&& other) noexcept {
        if (this != &other) {
            destroy();
            decoder_ = other.decoder_;
            msDecoder_ = other.msDecoder_;
            mapping_ = other.mapping_;
            sampleRate_ = other.sampleRate_;
            channels_ = other.channels_;
            lastError_ = other.lastError_;
            consecutiveErrors_ = other.consecutiveErrors_;
            other.decoder_ = nullptr;
            other.msDecoder_ = nullptr;
            other.mapping_ = OpusChannelMapping{};
            other.lastError_ = 0;
            other.consecutiveErrors_ = 0;
        }
        return *this;
    }

    OpusDecoderAdapter(const OpusDecoderAdapter&) = delete;
    OpusDecoderAdapter& operator=(const OpusDecoderAdapter&) = delete;

    /// `opusHead` is the track's CodecPrivate. Without it, only mono/stereo can
    /// be decoded; with it, multichannel layouts route through the multistream
    /// decoder, which is the only API that accepts channels > 2.
    bool initialize(int sampleRate, int channels,
                    const uint8_t* opusHead = nullptr,
                    size_t opusHeadSize = 0) noexcept {
        destroy();

        mapping_ = parseOpusHead(opusHead, opusHeadSize);
        if (mapping_.valid && mapping_.needsMultistream()) {
            return initializeMultistream(sampleRate, channels);
        }
        if (channels > 2) {
            // Multichannel without a usable mapping table cannot be decoded;
            // opus_decoder_create would reject it with OPUS_BAD_ARG anyway.
            lastError_ = OPUS_BAD_ARG;
            return false;
        }

        sampleRate_ = sampleRate;
        channels_ = channels;

        int error = 0;
        decoder_ = opus_decoder_create(sampleRate, channels, &error);

        if (error != OPUS_OK || !decoder_) {
            lastError_ = error;
            return false;
        }

        // OSCE (Deep PLC): LACE activates at complexity >= 6, NoLACE at >= 7.
        // DNN inference runs only during packet loss, not on every decoded frame.
        opus_decoder_ctl(decoder_, OPUS_SET_COMPLEXITY(config::decoder::kComplexity));

        return true;
    }

    // Decode an Opus packet into float PCM samples.
    // Returns number of decoded samples per channel, or -1 on error (check lastError()).
    int decode(const uint8_t* input, size_t inputSize, float* output, size_t maxFrames) noexcept {
        if (!input || !output) {
            lastError_ = OPUS_BAD_ARG;
            return -1;
        }
        if (!isValid()) {
            lastError_ = OPUS_INVALID_STATE;
            return -1;
        }

        // Validate sizes fit in int (opus API uses int for sizes)
        constexpr size_t kMaxInt = static_cast<size_t>(std::numeric_limits<int>::max());
        if (inputSize > kMaxInt || maxFrames > kMaxInt) {
            lastError_ = OPUS_BAD_ARG;
            return -1;
        }

        int frames = msDecoder_
            ? opus_multistream_decode_float(msDecoder_, input, static_cast<int>(inputSize),
                                            output, static_cast<int>(maxFrames), 0)
            : opus_decode_float(decoder_, input, static_cast<int>(inputSize),
                                output, static_cast<int>(maxFrames), 0);

        if (frames < 0) {
            lastError_ = frames;
            consecutiveErrors_++;
            return -1;
        }

        consecutiveErrors_ = 0;
        return frames;
    }

    // Generate PLC (packet loss concealment) output for a missing packet.
    // Uses Opus OSCE (Deep PLC) when complexity >= 7 for high-quality concealment.
    // Returns number of samples per channel, or -1 on error.
    int decodePLC(float* output, size_t maxFrames) noexcept {
        if (!output) {
            lastError_ = OPUS_BAD_ARG;
            return -1;
        }
        if (!isValid()) {
            lastError_ = OPUS_INVALID_STATE;
            return -1;
        }

        // Validate size fits in int (opus API uses int for sizes)
        constexpr size_t kMaxInt = static_cast<size_t>(std::numeric_limits<int>::max());
        if (maxFrames > kMaxInt) {
            lastError_ = OPUS_BAD_ARG;
            return -1;
        }

        int frames = msDecoder_
            ? opus_multistream_decode_float(msDecoder_, nullptr, 0, output,
                                            static_cast<int>(maxFrames), 0)
            : opus_decode_float(decoder_, nullptr, 0, output,
                                static_cast<int>(maxFrames), 0);

        if (frames < 0) {
            lastError_ = frames;
            consecutiveErrors_++;
            return -1;
        }

        consecutiveErrors_ = 0;
        return frames;
    }

    // Decode FEC (Forward Error Correction) data from a packet to recover the
    // PREVIOUS lost frame.  Opus embeds redundant lower-bitrate data for frame N
    // inside packet N+1.  Call this with packet N+1's data when frame N was lost,
    // BEFORE calling decode() on packet N+1 itself.
    // Returns number of recovered samples per channel, or -1 on error.
    int decodeFEC(const uint8_t* input, size_t inputSize, float* output, size_t maxFrames) noexcept {
        if (!input || !output) {
            lastError_ = OPUS_BAD_ARG;
            return -1;
        }
        if (!isValid()) {
            lastError_ = OPUS_INVALID_STATE;
            return -1;
        }

        constexpr size_t kMaxInt = static_cast<size_t>(std::numeric_limits<int>::max());
        if (inputSize > kMaxInt || maxFrames > kMaxInt) {
            lastError_ = OPUS_BAD_ARG;
            return -1;
        }

        int frames = msDecoder_
            ? opus_multistream_decode_float(msDecoder_, input, static_cast<int>(inputSize),
                                            output, static_cast<int>(maxFrames), 1)
            : opus_decode_float(decoder_, input, static_cast<int>(inputSize),
                                output, static_cast<int>(maxFrames), 1);

        if (frames < 0) {
            lastError_ = frames;
            consecutiveErrors_++;
            return -1;
        }

        consecutiveErrors_ = 0;
        return frames;
    }

    void reset() noexcept {
        if (msDecoder_) {
            opus_multistream_decoder_ctl(msDecoder_, OPUS_RESET_STATE);
        }
        if (decoder_) {
            opus_decoder_ctl(decoder_, OPUS_RESET_STATE);
        }
        consecutiveErrors_ = 0;
    }

    [[nodiscard]] int lastError() const noexcept { return lastError_; }
    [[nodiscard]] int consecutiveErrors() const noexcept { return consecutiveErrors_; }
    [[nodiscard]] bool isValid() const noexcept {
        return decoder_ != nullptr || msDecoder_ != nullptr;
    }

    /// Channel layout in force. `streams == 0` when nothing is initialised.
    [[nodiscard]] const OpusChannelMapping& channelMapping() const noexcept {
        return mapping_;
    }
    [[nodiscard]] int sampleRate() const noexcept { return sampleRate_; }
    [[nodiscard]] int channels() const noexcept { return channels_; }

private:
    void destroy() noexcept {
        if (decoder_) {
            opus_decoder_destroy(decoder_);
            decoder_ = nullptr;
        }
        if (msDecoder_) {
            opus_multistream_decoder_destroy(msDecoder_);
            msDecoder_ = nullptr;
        }
    }

    bool initializeMultistream(int sampleRate, int channels) noexcept {
        if (channels != mapping_.channels) {
            // The container and the OpusHead disagree; the mapping table is
            // sized by the header, so trusting the container would overrun it.
            lastError_ = OPUS_BAD_ARG;
            return false;
        }
        int error = OPUS_OK;
        msDecoder_ = opus_multistream_decoder_create(
            sampleRate, channels, mapping_.streams, mapping_.coupledStreams,
            mapping_.mapping, &error);
        if (error != OPUS_OK || !msDecoder_) {
            msDecoder_ = nullptr;
            lastError_ = error;
            return false;
        }
        opus_multistream_decoder_ctl(msDecoder_,
                                     OPUS_SET_COMPLEXITY(config::decoder::kComplexity));
        sampleRate_ = sampleRate;
        channels_ = channels;
        return true;
    }

    OpusDecoder* decoder_ = nullptr;
    OpusMSDecoder* msDecoder_ = nullptr;
    OpusChannelMapping mapping_{};
    int sampleRate_ = 0;
    int channels_ = 0;
    int lastError_ = 0;
    int consecutiveErrors_ = 0;

};

}  // namespace media
