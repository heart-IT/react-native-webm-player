#pragma once

#include <memory>
#include <cstring>
#include <limits>
#include <opus.h>
#include "common/MediaConfig.h"
#include "ComfortNoiseGenerator.h"

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
        , consecutiveErrors_(other.consecutiveErrors_)
        , dtxActive_(other.dtxActive_)
        , cng_(other.cng_) {
        other.decoder_ = nullptr;
        other.lastError_ = 0;
        other.consecutiveErrors_ = 0;
        other.dtxActive_ = false;
        other.cng_.reset();
    }

    OpusDecoderAdapter& operator=(OpusDecoderAdapter&& other) noexcept {
        if (this != &other) {
            destroy();
            decoder_ = other.decoder_;
            sampleRate_ = other.sampleRate_;
            channels_ = other.channels_;
            lastError_ = other.lastError_;
            consecutiveErrors_ = other.consecutiveErrors_;
            dtxActive_ = other.dtxActive_;
            cng_ = other.cng_;
            other.decoder_ = nullptr;
            other.lastError_ = 0;
            other.consecutiveErrors_ = 0;
            other.dtxActive_ = false;
            other.cng_.reset();
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

    static constexpr auto& kDtxFrame = config::dtx::kDtxFrame;

    int decode(const uint8_t* input, size_t inputSize, float* output, size_t maxFrames) noexcept {
        if (!input || !output) {
            lastError_ = OPUS_BAD_ARG;
            return -1;
        }
        if (!decoder_) {
            lastError_ = OPUS_INVALID_STATE;
            return -1;
        }

        // Handle DTX marker: explicit silence frame for discontinuous transmission
        if (inputSize == 3 &&
            input[0] == kDtxFrame[0] &&
            input[1] == kDtxFrame[1] &&
            input[2] == kDtxFrame[2]) {
            return decodeDtxSilence(output, maxFrames);
        }

        // Real audio frame — clear DTX state so next DTX entry uses PLC fade
        dtxActive_ = false;

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

    // Decode with FEC flag: attempts to extract redundancy from the packet for the previous frame.
    // With CELT-only packets (no embedded FEC), falls back to PLC-quality concealment.
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

    // Decode DTX silence marker with overlap-add state management.
    // First DTX frame after real audio uses PLC to fade the MDCT overlap
    // to silence, preventing a click when real audio resumes after long gaps.
    // Subsequent DTX frames output comfort noise (~-60dBFS shaped noise).
    int decodeDtxSilence(float* output, size_t maxFrames) noexcept {
        if (!output) {
            lastError_ = OPUS_BAD_ARG;
            return -1;
        }

        size_t framesToOutput = std::min(maxFrames, static_cast<size_t>(config::audio::kFrameSamples));

        if (!dtxActive_) {
            // First DTX frame after real audio: use PLC to fade overlap-add
            // to silence. Prevents onset click when real audio resumes.
            dtxActive_ = true;
            if (decoder_) {
                int plcFrames = opus_decode_float(decoder_, nullptr, 0,
                                                  output, static_cast<int>(framesToOutput), 0);
                if (plcFrames > 0) {
                    consecutiveErrors_ = 0;
                    return plcFrames;
                }
            }
        }

        // Subsequent DTX frames: overlap-add already clean, generate comfort noise.
        // Low-level shaped noise (~-60dBFS) prevents "dead air" perception.
        size_t samplesToOutput = framesToOutput * static_cast<size_t>(channels_);
        cng_.generate(output, samplesToOutput);
        return static_cast<int>(framesToOutput);
    }

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

    void reset() noexcept {
        if (decoder_) {
            opus_decoder_ctl(decoder_, OPUS_RESET_STATE);
        }
        consecutiveErrors_ = 0;
        dtxActive_ = false;
        cng_.reset();
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
    bool dtxActive_ = false;
    ComfortNoiseGenerator cng_;

};

}  // namespace media
