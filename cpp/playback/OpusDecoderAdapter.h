// Opus 1.6.1 decoder wrapper.
// Thread safety: NOT thread-safe. All calls must be from the decode thread.
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <opus.h>

namespace media {

class OpusDecoderAdapter {
public:
    OpusDecoderAdapter() = default;
    ~OpusDecoderAdapter() noexcept { destroy(); }

    OpusDecoderAdapter(const OpusDecoderAdapter&) = delete;
    OpusDecoderAdapter& operator=(const OpusDecoderAdapter&) = delete;

    // The broadcast is mono or stereo. opus_decoder_create rejects any other
    // channel count, and any rate but 8/12/16/24/48 kHz, with OPUS_BAD_ARG.
    bool initialize(int sampleRate, int channels) noexcept {
        destroy();
        int error = 0;
        decoder_ = opus_decoder_create(sampleRate, channels, &error);
        if (error != OPUS_OK || !decoder_) {
            lastError_ = error;
            return false;
        }
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
            return -1;
        }
        return frames;
    }

    void reset() noexcept {
        if (decoder_) {
            opus_decoder_ctl(decoder_, OPUS_RESET_STATE);
        }
    }

    [[nodiscard]] int lastError() const noexcept { return lastError_; }
    [[nodiscard]] bool isValid() const noexcept { return decoder_ != nullptr; }

private:
    void destroy() noexcept {
        if (decoder_) {
            opus_decoder_destroy(decoder_);
            decoder_ = nullptr;
        }
    }

    OpusDecoder* decoder_ = nullptr;
    int lastError_ = 0;
};

}  // namespace media
