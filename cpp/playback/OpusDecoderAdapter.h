// Opus 1.6.1 decoder wrapper with OSCE (Deep PLC) support.
// Thread safety: NOT thread-safe. All calls must be from the decode thread.
#pragma once

#include <memory>
#include <cstring>
#include <limits>
#include <opus.h>
#include "common/MediaConfig.h"

namespace media {

class OpusDecoderAdapter {
public:
    OpusDecoderAdapter() = default;
    ~OpusDecoderAdapter() noexcept { destroy(); }

    OpusDecoderAdapter(OpusDecoderAdapter&& other) noexcept
        : decoder_(other.decoder_)
        , sampleRate_(other.sampleRate_)
        , channels_(other.channels_)
        , lastError_(other.lastError_)
        , consecutiveErrors_(other.consecutiveErrors_) {
        other.decoder_ = nullptr;
        other.lastError_ = 0;
        other.consecutiveErrors_ = 0;
    }

    OpusDecoderAdapter& operator=(OpusDecoderAdapter&& other) noexcept {
        if (this != &other) {
            destroy();
            decoder_ = other.decoder_;
            sampleRate_ = other.sampleRate_;
            channels_ = other.channels_;
            lastError_ = other.lastError_;
            consecutiveErrors_ = other.consecutiveErrors_;
            other.decoder_ = nullptr;
            other.lastError_ = 0;
            other.consecutiveErrors_ = 0;
        }
        return *this;
    }

    OpusDecoderAdapter(const OpusDecoderAdapter&) = delete;
    OpusDecoderAdapter& operator=(const OpusDecoderAdapter&) = delete;

    bool initialize(int sampleRate, int channels) noexcept {
        destroy();

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
        if (!decoder_) {
            lastError_ = OPUS_INVALID_STATE;
            return -1;
        }

        // Validate sizes fit in int (opus API uses int for sizes)
        constexpr size_t kMaxInt = static_cast<size_t>(std::numeric_limits<int>::max());
        if (inputSize > kMaxInt || maxFrames > kMaxInt) {
            lastError_ = OPUS_BAD_ARG;
            return -1;
        }

        int frames = opus_decode_float(decoder_, input, static_cast<int>(inputSize),
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
        if (!decoder_) {
            lastError_ = OPUS_INVALID_STATE;
            return -1;
        }

        // Validate size fits in int (opus API uses int for sizes)
        constexpr size_t kMaxInt = static_cast<size_t>(std::numeric_limits<int>::max());
        if (maxFrames > kMaxInt) {
            lastError_ = OPUS_BAD_ARG;
            return -1;
        }

        int frames = opus_decode_float(decoder_, nullptr, 0,
                                       output, static_cast<int>(maxFrames), 0);

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
        if (!decoder_) {
            lastError_ = OPUS_INVALID_STATE;
            return -1;
        }

        constexpr size_t kMaxInt = static_cast<size_t>(std::numeric_limits<int>::max());
        if (inputSize > kMaxInt || maxFrames > kMaxInt) {
            lastError_ = OPUS_BAD_ARG;
            return -1;
        }

        int frames = opus_decode_float(decoder_, input, static_cast<int>(inputSize),
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
        if (decoder_) {
            opus_decoder_ctl(decoder_, OPUS_RESET_STATE);
        }
        consecutiveErrors_ = 0;
    }

    [[nodiscard]] int lastError() const noexcept { return lastError_; }
    [[nodiscard]] int consecutiveErrors() const noexcept { return consecutiveErrors_; }
    [[nodiscard]] bool isValid() const noexcept { return decoder_ != nullptr; }
    [[nodiscard]] int sampleRate() const noexcept { return sampleRate_; }
    [[nodiscard]] int channels() const noexcept { return channels_; }

private:
    void destroy() noexcept {
        if (decoder_) {
            opus_decoder_destroy(decoder_);
            decoder_ = nullptr;
        }
    }

    OpusDecoder* decoder_ = nullptr;
    int sampleRate_ = 0;
    int channels_ = 0;
    int lastError_ = 0;
    int consecutiveErrors_ = 0;

};

}  // namespace media
