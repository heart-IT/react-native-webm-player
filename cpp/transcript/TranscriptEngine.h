// Wrapper around whisper.cpp for on-device speech transcription.
//
// Handles model loading, 48kHz->16kHz downsampling (via SpeexDSP),
// and inference. All methods are blocking -- call from transcript thread only.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>
#include "common/AudioResampler.h"
#include "common/MediaLog.h"
#include "transcript/TranscriptRegistry.h"

struct whisper_context;
struct whisper_full_params;

namespace media::transcript {

namespace config {
    constexpr int kWhisperSampleRate = 16000;
    constexpr int kSourceSampleRate = 48000;
    constexpr int kDownsampleRatio = kSourceSampleRate / kWhisperSampleRate;  // 3
    constexpr int kWhisperThreads = 4;
    constexpr float kMaxChunkSeconds = 5.0f;
    constexpr size_t kMaxChunkSamples = static_cast<size_t>(kWhisperSampleRate * kMaxChunkSeconds);
    constexpr size_t kResampleBufferSize = kMaxChunkSamples + 256;
}

class TranscriptEngine {
public:
    TranscriptEngine() = default;
    ~TranscriptEngine() { shutdown(); }

    TranscriptEngine(const TranscriptEngine&) = delete;
    TranscriptEngine& operator=(const TranscriptEngine&) = delete;

    bool init(const std::string& modelPath) {
        if (ctx_) shutdown();

        ctx_ = loadModel(modelPath);
        if (!ctx_) {
            MEDIA_LOG_E("TranscriptEngine: failed to load model: %s", modelPath.c_str());
            return false;
        }

        if (!resampler_.init(config::kSourceSampleRate, config::kWhisperSampleRate)) {
            MEDIA_LOG_E("TranscriptEngine: resampler init failed");
            shutdown();
            return false;
        }

        resampleBuf_.resize(config::kResampleBufferSize);
        MEDIA_LOG_I("TranscriptEngine: initialized with model %s", modelPath.c_str());
        return true;
    }

    void shutdown() {
        if (ctx_) {
            freeModel(ctx_);
            ctx_ = nullptr;
        }
        resampler_.destroy();
        resampleBuf_.clear();
    }

    [[nodiscard]] bool isInitialized() const noexcept { return ctx_ != nullptr; }

    // Translate to English instead of transcribing in source language.
    // Zero-cost: uses the same model, same inference pass.
    void setTranslateToEnglish(bool translate) noexcept {
        translateToEnglish_.store(translate, std::memory_order_relaxed);
    }

    [[nodiscard]] bool translateToEnglish() const noexcept {
        return translateToEnglish_.load(std::memory_order_relaxed);
    }

    std::vector<TranscriptSegment> process(const float* samples48k, size_t count,
                                            int64_t startTimeUs) {
        std::vector<TranscriptSegment> segments;
        if (!ctx_ || count == 0) return segments;

        size_t outCount = resampler_.process(resampleBuf_.data(), samples48k, count);
        if (outCount == 0) return segments;

        if (outCount > config::kMaxChunkSamples) {
            outCount = config::kMaxChunkSamples;
        }

        bool translate = translateToEnglish_.load(std::memory_order_relaxed);
        int nSegments = runInference(ctx_, resampleBuf_.data(), outCount, translate);

        for (int i = 0; i < nSegments; ++i) {
            TranscriptSegment seg;
            seg.text = getSegmentText(ctx_, i);
            seg.startUs = startTimeUs + getSegmentStartMs(ctx_, i) * 1000;
            seg.endUs = startTimeUs + getSegmentEndMs(ctx_, i) * 1000;
            seg.isFinal = true;
            if (!seg.text.empty()) {
                segments.push_back(std::move(seg));
            }
        }

        return segments;
    }

private:
    static whisper_context* loadModel(const std::string& path);
    static void freeModel(whisper_context* ctx);
    static int runInference(whisper_context* ctx, const float* samples, size_t count, bool translate);
    static const char* getSegmentText(whisper_context* ctx, int index);
    static int64_t getSegmentStartMs(whisper_context* ctx, int index);
    static int64_t getSegmentEndMs(whisper_context* ctx, int index);

    whisper_context* ctx_ = nullptr;
    AudioResampler<float> resampler_;
    std::vector<float> resampleBuf_;
    std::atomic<bool> translateToEnglish_{false};
};

}  // namespace media::transcript
