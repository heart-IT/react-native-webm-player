// Android AAudio output bridge — platform-specific stream operations.
// Derives shared state machine, restart loop, and render core from AudioOutputBridgeBase.
#pragma once

#include "common/AudioOutputBridgeBase.h"
#include <aaudio/AAudio.h>

namespace media {

struct AAudioConfig : AudioOutputConfig {
    aaudio_performance_mode_t performanceMode = AAUDIO_PERFORMANCE_MODE_LOW_LATENCY;
    aaudio_sharing_mode_t sharingMode = AAUDIO_SHARING_MODE_EXCLUSIVE;
    aaudio_usage_t usage = AAUDIO_USAGE_MEDIA;
    aaudio_content_type_t contentType = AAUDIO_CONTENT_TYPE_MOVIE;
};

class AAudioOutputBridge : public AudioOutputBridgeBase<AAudioOutputBridge> {
    friend class AudioOutputBridgeBase<AAudioOutputBridge>;
public:
    explicit AAudioOutputBridge(AudioCallback callback, AAudioConfig config = {}) noexcept
        : AudioOutputBridgeBase(std::move(callback),
                                {config.sampleRate, config.channelCount, config.framesPerBuffer})
        , aaudioConfig_(config) {}

    ~AAudioOutputBridge() noexcept { stop(); }

    // NOTE: Acquires mutex to safely access stream_ pointer.
    // Only call from metrics/diagnostics path, not from audio callback.
    [[nodiscard]] int64_t totalLatencyUs() const noexcept {
        std::lock_guard<std::mutex> lk(streamMtx_);
        AAudioStream* s = stream_.load(std::memory_order_relaxed);
        if (!s) return 0;

        int64_t frames = 0, time = 0;
        if (AAudioStream_getTimestamp(s, CLOCK_MONOTONIC, &frames, &time) != AAUDIO_OK) return 0;

        int64_t bufferFrames = AAudioStream_getBufferSizeInFrames(s);
        return (bufferFrames * 1000000LL) / baseConfig_.sampleRate;
    }

    [[nodiscard]] int32_t actualSampleRate() const noexcept {
        std::lock_guard<std::mutex> lk(streamMtx_);
        AAudioStream* s = stream_.load(std::memory_order_relaxed);
        return s ? AAudioStream_getSampleRate(s) : 0;
    }

    [[nodiscard]] aaudio_result_t lastError() const noexcept {
        return lastError_.load(std::memory_order_relaxed);
    }

private:
    // ── CRTP interface for AudioOutputBridgeBase ──

    const char* bridgeName() const noexcept { return "AAudioOutputBridge"; }
    const char* restartThreadName() const noexcept { return "AAudioOutRstrt"; }
    int restartDelayMs() const noexcept { return config::audio_output::kRestartDelayMs; }

    void storeLastError(int code) noexcept {
        lastError_.store(static_cast<aaudio_result_t>(code), std::memory_order_relaxed);
    }

    void logRestartError() noexcept {
        aaudio_result_t err = lastError_.load(std::memory_order_relaxed);
        if (err != AAUDIO_OK) {
            MEDIA_LOG_E("AAudio output error: %d (%s)%s", err,
                        AAudio_convertResultToText(err),
                        isRecoverableError(err) ? ", restarting" : ", fatal");
        }
    }

    // ── Platform stream operations ──

    [[nodiscard]] AAudioStream* tryOpenStream(aaudio_sharing_mode_t sharingMode) noexcept {
        AAudioStreamBuilder* builder = nullptr;
        aaudio_result_t result = AAudio_createStreamBuilder(&builder);
        if (result != AAUDIO_OK) {
            MEDIA_LOG_E("AAudio createStreamBuilder failed: %d", result);
            lastError_.store(result, std::memory_order_relaxed);
            return nullptr;
        }

        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setSampleRate(builder, baseConfig_.sampleRate);
        AAudioStreamBuilder_setChannelCount(builder, baseConfig_.channelCount);
        AAudioStreamBuilder_setPerformanceMode(builder, aaudioConfig_.performanceMode);
        AAudioStreamBuilder_setSharingMode(builder, sharingMode);
        int32_t callbackFrames = baseConfig_.framesPerBuffer > 0
            ? baseConfig_.framesPerBuffer
            : AAUDIO_UNSPECIFIED;
        AAudioStreamBuilder_setFramesPerDataCallback(builder, callbackFrames);
        AAudioStreamBuilder_setDataCallback(builder, dataCallback, this);
        AAudioStreamBuilder_setErrorCallback(builder, errorCallback, this);
        AAudioStreamBuilder_setUsage(builder, aaudioConfig_.usage);
        AAudioStreamBuilder_setContentType(builder, aaudioConfig_.contentType);

        AAudioStream* newStream = nullptr;
        result = AAudioStreamBuilder_openStream(builder, &newStream);
        AAudioStreamBuilder_delete(builder);

        if (result != AAUDIO_OK) {
            MEDIA_LOG_W("AAudio openStream failed (sharing=%d): %d", sharingMode, result);
            lastError_.store(result, std::memory_order_relaxed);
            return nullptr;
        }

        return newStream;
    }

    [[nodiscard]] bool createStreamImpl() noexcept {
        std::lock_guard<std::mutex> lk(streamMtx_);

        AAudioStream* newStream = nullptr;

        if (aaudioConfig_.sharingMode == AAUDIO_SHARING_MODE_EXCLUSIVE) {
            newStream = tryOpenStream(AAUDIO_SHARING_MODE_EXCLUSIVE);

            if (newStream) {
                int32_t actualRate = AAudioStream_getSampleRate(newStream);
                if (actualRate != baseConfig_.sampleRate) {
                    MEDIA_LOG_W("AAudio exclusive: rate mismatch (requested=%d actual=%d), trying shared",
                                baseConfig_.sampleRate, actualRate);
                    AAudioStream_close(newStream);
                    newStream = nullptr;
                }
            }

            if (!newStream) {
                MEDIA_LOG_I("AAudio: falling back to shared mode");
                newStream = tryOpenStream(AAUDIO_SHARING_MODE_SHARED);
            }
        } else {
            newStream = tryOpenStream(aaudioConfig_.sharingMode);
        }

        if (!newStream) {
            MEDIA_LOG_E("AAudio: failed to open stream in any mode");
            return false;
        }

        int32_t burstSize = AAudioStream_getFramesPerBurst(newStream);
        int32_t desiredBuffer = burstSize * config::aaudio::kBufferBurstMultiplier;
        AAudioStream_setBufferSizeInFrames(newStream, desiredBuffer);

        int32_t actualBuffer = AAudioStream_getBufferSizeInFrames(newStream);
        int32_t actualRate = AAudioStream_getSampleRate(newStream);
        int32_t actualChannels = AAudioStream_getChannelCount(newStream);
        aaudio_sharing_mode_t grantedSharing = AAudioStream_getSharingMode(newStream);

        if (!playbackResampler_.init(baseConfig_.sampleRate, actualRate)) {
            MEDIA_LOG_E("AAudio: unsupported rate conversion (%d -> %d)",
                        baseConfig_.sampleRate, actualRate);
            AAudioStream_close(newStream);
            lastError_.store(AAUDIO_ERROR_INVALID_FORMAT, std::memory_order_relaxed);
            return false;
        }
        grantedSampleRate_.store(actualRate, std::memory_order_relaxed);

        if (actualChannels != baseConfig_.channelCount) {
            MEDIA_LOG_E("AAudio channel mismatch: requested=%d actual=%d",
                        baseConfig_.channelCount, actualChannels);
            AAudioStream_close(newStream);
            lastError_.store(AAUDIO_ERROR_INVALID_FORMAT, std::memory_order_relaxed);
            return false;
        }

        LatencyMode latencyMode = (grantedSharing == AAUDIO_SHARING_MODE_EXCLUSIVE)
                                  ? LatencyMode::LowLatency
                                  : LatencyMode::Standard;
        grantedLatencyMode_.store(latencyMode, std::memory_order_relaxed);

        int32_t callbackFrames = AAudioStream_getFramesPerDataCallback(newStream);

        MEDIA_LOG_I("AAudio output: rate=%d ch=%d burst=%d buffer=%d callback=%d sharing=%d resample=%s",
                    actualRate, actualChannels, burstSize, actualBuffer,
                    callbackFrames, grantedSharing,
                    playbackResampler_.isPassthrough() ? "none" : "active");

        stream_.store(newStream, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool startStreamImpl() noexcept {
        std::lock_guard<std::mutex> lk(streamMtx_);
        AAudioStream* s = stream_.load(std::memory_order_relaxed);
        if (!s) {
            MEDIA_LOG_E("AAudio startStream: no stream");
            return false;
        }

        aaudio_result_t result = AAudioStream_requestStart(s);
        if (result != AAUDIO_OK) {
            MEDIA_LOG_E("AAudio requestStart failed: %d", result);
            lastError_.store(result, std::memory_order_relaxed);
            return false;
        }
        MEDIA_LOG_I("AAudio stream started");
        return true;
    }

    void destroyStreamImpl() noexcept {
        std::lock_guard<std::mutex> lk(streamMtx_);
        AAudioStream* s = stream_.exchange(nullptr, std::memory_order_acq_rel);
        if (s) {
            AAudioStream_requestStop(s);
            AAudioStream_close(s);
        }
    }

    // ── AAudio callbacks ──

    HOT_FUNCTION
    static aaudio_data_callback_result_t dataCallback(
            AAudioStream* stream, void* userData, void* audioData, int32_t numFrames) {
        auto* self = static_cast<AAudioOutputBridge*>(userData);
        auto* output = static_cast<float*>(audioData);

        if (UNLIKELY(numFrames <= 0 || !output)) {
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        }

        // Guard against stale stream callbacks after restart
        AAudioStream* currentStream = self->stream_.load(std::memory_order_acquire);
        if (currentStream != stream) {
            std::memset(output, 0, static_cast<size_t>(numFrames) * static_cast<size_t>(self->baseConfig_.channelCount) * sizeof(float));
            return AAUDIO_CALLBACK_RESULT_STOP;
        }

        (void)self->renderAudioCore(output, static_cast<size_t>(numFrames));

        // Return STOP if state is not active/warmup (renderAudioCore returned 0 for non-active)
        uint8_t state = self->callbackState_.load(std::memory_order_acquire);
        if (state != kStateActive && state != kStateWarmUp) {
            return AAUDIO_CALLBACK_RESULT_STOP;
        }
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    static void errorCallback(AAudioStream* stream, void* userData, aaudio_result_t error) {
        auto* self = static_cast<AAudioOutputBridge*>(userData);

        uint8_t state = self->callbackState_.load(std::memory_order_acquire);
        if (state != kStateActive && state != kStateWarmUp) return;

        AAudioStream* currentStream = self->stream_.load(std::memory_order_acquire);
        if (currentStream != stream) return;

        self->lastError_.store(error, std::memory_order_relaxed);

        if (isRecoverableError(error)) {
            self->scheduleRestart();
        } else {
            self->callbackState_.store(kStateStopped, std::memory_order_release);
            self->scheduleRestart();
        }
    }

    static bool isRecoverableError(aaudio_result_t error) noexcept {
        switch (error) {
            case AAUDIO_ERROR_DISCONNECTED:
            case AAUDIO_ERROR_TIMEOUT:
            case AAUDIO_ERROR_UNAVAILABLE:
            case AAUDIO_ERROR_INVALID_STATE:
            case AAUDIO_ERROR_WOULD_BLOCK:
                return true;
            default:
                return false;
        }
    }

    // ── Platform-specific state ──

    const AAudioConfig aaudioConfig_;
    mutable std::mutex streamMtx_;
    std::atomic<AAudioStream*> stream_{nullptr};
    std::atomic<aaudio_result_t> lastError_{AAUDIO_OK};
};

}  // namespace media
