// Threading model: Lock-free audio callback path, mutex-protected lifecycle operations
#pragma once

#include "common/ThreadAffinity.h"
#include "common/MediaLog.h"
#include "common/AudioResampler.h"
#include <aaudio/AAudio.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <functional>
#include <cstring>
#include "common/MediaConfig.h"
#include "common/MediaTime.h"
#include "common/TimedJoin.h"
#include "MediaTypes.h"

namespace media {

struct AAudioConfig {
    int32_t sampleRate = config::audio::kSampleRate;
    int32_t channelCount = config::audio::kChannels;
    int32_t framesPerBuffer = config::audio::kFrameSamples;
    aaudio_performance_mode_t performanceMode = AAUDIO_PERFORMANCE_MODE_LOW_LATENCY;
    aaudio_sharing_mode_t sharingMode = AAUDIO_SHARING_MODE_EXCLUSIVE;
    aaudio_usage_t usage = AAUDIO_USAGE_MEDIA;
    aaudio_content_type_t contentType = AAUDIO_CONTENT_TYPE_MOVIE;
};

class AAudioOutputBridge {
public:
    using AudioCallback = std::function<size_t(float* output, size_t frameCount)>;
    using RestartCallback = std::function<void()>;

    static constexpr uint8_t kStateStopped = 0;
    static constexpr uint8_t kStateWarmUp = 1;      // Stream running with silence (hardware hot)
    static constexpr uint8_t kStateActive = 2;
    static constexpr uint8_t kStateShuttingDown = 3;
    static constexpr int kMaxRestartRetries = 5;
    static constexpr int kMaxBackoffMs = 3200;

    explicit AAudioOutputBridge(AudioCallback callback, AAudioConfig config = {}) noexcept
        : callback_(std::move(callback))
        , config_(config) {}

    ~AAudioOutputBridge() noexcept { stop(); }

    AAudioOutputBridge(const AAudioOutputBridge&) = delete;
    AAudioOutputBridge& operator=(const AAudioOutputBridge&) = delete;

    void setRestartCallback(RestartCallback cb) noexcept {
        std::lock_guard<std::mutex> lk(callbackMtx_);
        restartCallback_ = std::move(cb);
    }

    // Set pointer to AudioSessionManager's interrupted flag for RT-safe
    // muting during audio focus loss. Mirrors iOS setInterruptedFlag().
    void setInterruptedFlag(std::atomic<bool>* flag) noexcept { interruptedFlag_ = flag; }

    // Pre-activate AAudio stream with silence to reduce first-frame latency.
    // Call before audio is expected, then call start() when ready for real audio.
    [[nodiscard]] bool warmUp() noexcept {
        std::lock_guard<std::mutex> lk(lifecycleMtx_);

        uint8_t expected = kStateStopped;
        if (!callbackState_.compare_exchange_strong(expected, kStateWarmUp,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            // Already warming up or active - success
            uint8_t current = callbackState_.load(std::memory_order_acquire);
            return current == kStateWarmUp || current == kStateActive;
        }

        if (!createStream()) {
            callbackState_.store(kStateStopped, std::memory_order_release);
            return false;
        }

        restartThreadExited_.store(false, std::memory_order_relaxed);
        restartThread_ = std::thread([this] { restartLoop(); });

        if (!startStream()) {
            restartRequested_.store(true, std::memory_order_release);
            restartRequested_.notify_one();
            callbackState_.store(kStateShuttingDown, std::memory_order_release);
            timedJoin(restartThread_, restartThreadExited_,
                      config::thread::kThreadJoinTimeoutMs, "AAudioOutputBridge::warmUp");
            destroyStream();
            callbackState_.store(kStateStopped, std::memory_order_release);
            return false;
        }

        consecutiveRestartFailures_.store(0, std::memory_order_relaxed);
        MEDIA_LOG_I("AAudioOutputBridge: warm-up complete, hardware hot");
        return true;
    }

    [[nodiscard]] bool start() noexcept {
        std::lock_guard<std::mutex> lk(lifecycleMtx_);

        uint8_t expected = kStateWarmUp;

        // If already warmed up, transition to active (CAS to avoid racing with error callback)
        if (callbackState_.compare_exchange_strong(expected, kStateActive,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            MEDIA_LOG_I("AAudioOutputBridge: warm-up -> active (zero latency transition)");
            return true;
        }

        // If already active, nothing to do
        if (expected == kStateActive) {
            return true;
        }

        // Cold start path - full initialization
        expected = kStateStopped;
        if (!callbackState_.compare_exchange_strong(expected, kStateActive,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return false;
        }

        if (!createStream()) {
            callbackState_.store(kStateStopped, std::memory_order_release);
            return false;
        }

        restartThreadExited_.store(false, std::memory_order_relaxed);
        restartThread_ = std::thread([this] { restartLoop(); });

        if (!startStream()) {
            // Signal restart thread to exit before cleanup
            restartRequested_.store(true, std::memory_order_release);
            restartRequested_.notify_one();

            // Transition to shutting down so restart thread exits its loop
            callbackState_.store(kStateShuttingDown, std::memory_order_release);

            timedJoin(restartThread_, restartThreadExited_,
                      config::thread::kThreadJoinTimeoutMs, "AAudioOutputBridge::start");

            destroyStream();
            callbackState_.store(kStateStopped, std::memory_order_release);
            return false;
        }

        consecutiveRestartFailures_.store(0, std::memory_order_relaxed);
        return true;
    }

    void stop() noexcept {
        {
            std::lock_guard<std::mutex> lk(lifecycleMtx_);
            uint8_t current = callbackState_.load(std::memory_order_acquire);

            // Already stopped, nothing to do
            if (current == kStateStopped) return;

            // Transition from warm-up or active to shutting down
            if (current == kStateWarmUp || current == kStateActive) {
                callbackState_.store(kStateShuttingDown, std::memory_order_release);
            } else if (current == kStateShuttingDown) {
                // Already shutting down
                return;
            }
        }

        restartRequested_.store(true, std::memory_order_release);
        restartRequested_.notify_one();

        timedJoin(restartThread_, restartThreadExited_,
                  config::thread::kThreadJoinTimeoutMs, "AAudioOutputBridge::stop");
        destroyStream();
        
        callbackState_.store(kStateStopped, std::memory_order_release);
    }

    [[nodiscard]] bool isActive() const noexcept {
        return callbackState_.load(std::memory_order_acquire) == kStateActive;
    }

    [[nodiscard]] bool isWarmingUp() const noexcept {
        return callbackState_.load(std::memory_order_acquire) == kStateWarmUp;
    }

    [[nodiscard]] bool isRunning() const noexcept {
        uint8_t state = callbackState_.load(std::memory_order_acquire);
        return state == kStateWarmUp || state == kStateActive;
    }

    void pauseToWarmUp() noexcept {
        uint8_t expected = kStateActive;
        callbackState_.compare_exchange_strong(expected, kStateWarmUp,
            std::memory_order_acq_rel, std::memory_order_relaxed);
    }

    void resumeFromWarmUp() noexcept {
        uint8_t expected = kStateWarmUp;
        callbackState_.compare_exchange_strong(expected, kStateActive,
            std::memory_order_acq_rel, std::memory_order_relaxed);
    }

    [[nodiscard]] int64_t framesWritten() const noexcept {
        return framesWritten_.load(std::memory_order_relaxed);
    }

    // NOTE: Acquires mutex to safely access stream_ pointer.
    // Only call from metrics/diagnostics path, not from audio callback.
    // Called from JS thread via getMetrics(), not from hot path.
    [[nodiscard]] int64_t totalLatencyUs() const noexcept {
        std::lock_guard<std::mutex> lk(streamMtx_);
        AAudioStream* s = stream_.load(std::memory_order_relaxed);
        if (!s) return 0;

        int64_t frames = 0, time = 0;
        if (AAudioStream_getTimestamp(s, CLOCK_MONOTONIC, &frames, &time) != AAUDIO_OK) return 0;

        int64_t bufferFrames = AAudioStream_getBufferSizeInFrames(s);
        return (bufferFrames * 1000000LL) / config_.sampleRate;
    }

    [[nodiscard]] int32_t actualSampleRate() const noexcept {
        std::lock_guard<std::mutex> lk(streamMtx_);
        AAudioStream* s = stream_.load(std::memory_order_relaxed);
        return s ? AAudioStream_getSampleRate(s) : 0;
    }

    [[nodiscard]] aaudio_result_t lastError() const noexcept {
        return lastError_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint32_t restartCount() const noexcept {
        return restartCount_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] int64_t callbackJitterUs() const noexcept {
        return callbackJitterUs_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool isSampleRateValid() const noexcept {
        int32_t granted = grantedSampleRate_.load(std::memory_order_relaxed);
        return granted == 0 || granted == config_.sampleRate;
    }

    [[nodiscard]] LatencyMode grantedLatencyMode() const noexcept {
        return grantedLatencyMode_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool isLowLatencyMode() const noexcept {
        return grantedLatencyMode_.load(std::memory_order_relaxed) == LatencyMode::LowLatency;
    }

    void scheduleRestart() noexcept {
        restartRequested_.store(true, std::memory_order_release);
        restartRequested_.notify_one();
    }

private:
    // Attempts to open stream with given sharing mode, returns stream or nullptr
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
        AAudioStreamBuilder_setSampleRate(builder, config_.sampleRate);
        AAudioStreamBuilder_setChannelCount(builder, config_.channelCount);
        AAudioStreamBuilder_setPerformanceMode(builder, config_.performanceMode);
        AAudioStreamBuilder_setSharingMode(builder, sharingMode);
        int32_t callbackFrames = config_.framesPerBuffer > 0 
            ? config_.framesPerBuffer 
            : AAUDIO_UNSPECIFIED;
        AAudioStreamBuilder_setFramesPerDataCallback(builder, callbackFrames);
        AAudioStreamBuilder_setDataCallback(builder, dataCallback, this);
        AAudioStreamBuilder_setErrorCallback(builder, errorCallback, this);
        AAudioStreamBuilder_setUsage(builder, config_.usage);
        AAudioStreamBuilder_setContentType(builder, config_.contentType);

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

    [[nodiscard]] bool createStream() noexcept {
        std::lock_guard<std::mutex> lk(streamMtx_);

        // Strategy: Try exclusive mode first (lowest latency), fall back to shared mode
        // if exclusive fails or returns incompatible format
        AAudioStream* newStream = nullptr;
        
        if (config_.sharingMode == AAUDIO_SHARING_MODE_EXCLUSIVE) {
            newStream = tryOpenStream(AAUDIO_SHARING_MODE_EXCLUSIVE);
            
            if (newStream) {
                int32_t actualSampleRate = AAudioStream_getSampleRate(newStream);
                if (actualSampleRate != config_.sampleRate) {
                    MEDIA_LOG_W("AAudio exclusive mode: sample rate mismatch (requested=%d actual=%d), trying shared",
                                config_.sampleRate, actualSampleRate);
                    AAudioStream_close(newStream);
                    newStream = nullptr;
                }
            }
            
            // Fallback to shared mode
            if (!newStream) {
                MEDIA_LOG_I("AAudio: falling back to shared mode");
                newStream = tryOpenStream(AAUDIO_SHARING_MODE_SHARED);
            }
        } else {
            newStream = tryOpenStream(config_.sharingMode);
        }
        
        if (!newStream) {
            MEDIA_LOG_E("AAudio: failed to open stream in any mode");
            return false;
        }

        int32_t burstSize = AAudioStream_getFramesPerBurst(newStream);
        int32_t desiredBuffer = burstSize * config::aaudio::kBufferBurstMultiplier;
        AAudioStream_setBufferSizeInFrames(newStream, desiredBuffer);
        
        int32_t actualBuffer = AAudioStream_getBufferSizeInFrames(newStream);
        int32_t actualSampleRate = AAudioStream_getSampleRate(newStream);
        int32_t actualChannels = AAudioStream_getChannelCount(newStream);
        aaudio_sharing_mode_t grantedSharing = AAudioStream_getSharingMode(newStream);
        
        // Initialize resampler for rate mismatch (e.g., Opus 48kHz → SCO 16kHz)
        if (!playbackResampler_.init(config_.sampleRate, actualSampleRate)) {
            MEDIA_LOG_E("AAudio: unsupported rate conversion (%d -> %d)",
                        config_.sampleRate, actualSampleRate);
            AAudioStream_close(newStream);
            lastError_.store(AAUDIO_ERROR_INVALID_FORMAT, std::memory_order_relaxed);
            return false;
        }
        grantedSampleRate_.store(actualSampleRate, std::memory_order_relaxed);

        if (actualChannels != config_.channelCount) {
            MEDIA_LOG_E("AAudio channel count mismatch: requested=%d actual=%d (incompatible)",
                        config_.channelCount, actualChannels);
            AAudioStream_close(newStream);
            lastError_.store(AAUDIO_ERROR_INVALID_FORMAT, std::memory_order_relaxed);
            return false;
        }
        
        if (grantedSharing != config_.sharingMode) {
            MEDIA_LOG_I("AAudio sharing mode: requested=%d granted=%d",
                        config_.sharingMode, grantedSharing);
        }
        
        // Track granted latency mode for diagnostics
        LatencyMode latencyMode = (grantedSharing == AAUDIO_SHARING_MODE_EXCLUSIVE) 
                                  ? LatencyMode::LowLatency 
                                  : LatencyMode::Standard;
        grantedLatencyMode_.store(latencyMode, std::memory_order_relaxed);
        
        int32_t callbackFrames = AAudioStream_getFramesPerDataCallback(newStream);

        MEDIA_LOG_I("AAudio output stream: rate=%d ch=%d burst=%d buffer=%d callback=%d sharing=%d resample=%s",
                    actualSampleRate, actualChannels, burstSize, actualBuffer,
                    callbackFrames, grantedSharing,
                    playbackResampler_.isPassthrough() ? "none" : "active");

        stream_.store(newStream, std::memory_order_release);

        return true;
    }

    [[nodiscard]] bool startStream() noexcept {
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

    void destroyStream() noexcept {
        std::lock_guard<std::mutex> lk(streamMtx_);
        AAudioStream* s = stream_.exchange(nullptr, std::memory_order_acq_rel);
        if (s) {
            AAudioStream_requestStop(s);
            AAudioStream_close(s);
        }
    }

    void restartLoop() noexcept {
        thread_affinity::setThreadName("AAudioOutRstrt");
        auto cfg = thread_affinity::configureCurrentThreadForWorker();
        if (!cfg.niceSet) {
            MEDIA_LOG_W("AAudioOutputBridge: restart thread priority not set");
        }

        while (callbackState_.load(std::memory_order_acquire) != kStateShuttingDown) {
            // Wait for restart request using C++20 atomic wait
            while (!restartRequested_.load(std::memory_order_acquire)) {
                if (callbackState_.load(std::memory_order_acquire) == kStateShuttingDown) {
                    restartThreadExited_.store(true, std::memory_order_release);
                    return;
                }
                restartRequested_.wait(false, std::memory_order_relaxed);
                // Recheck shutdown after wake — notification from stop() may have arrived
                // between the state check above and the wait() call
                if (callbackState_.load(std::memory_order_acquire) == kStateShuttingDown) {
                    restartThreadExited_.store(true, std::memory_order_release);
                    return;
                }
            }

            if (callbackState_.load(std::memory_order_acquire) == kStateShuttingDown) break;
            restartRequested_.store(false, std::memory_order_release);

            // Log error deferred from RT error callback
            aaudio_result_t err = lastError_.load(std::memory_order_relaxed);
            if (err != AAUDIO_OK) {
                MEDIA_LOG_E("AAudio output error: %d (%s)%s", err,
                            AAudio_convertResultToText(err),
                            isRecoverableError(err) ? ", restarting" : ", fatal");
            }

            uint8_t state = callbackState_.load(std::memory_order_acquire);
            if (state != kStateActive && state != kStateWarmUp) continue;

            destroyStream();
            
            std::this_thread::sleep_for(std::chrono::milliseconds(config::aaudio::kRestartDelayMs));

            if (!createStream() || !startStream()) {
                int failures = consecutiveRestartFailures_.fetch_add(1, std::memory_order_relaxed) + 1;
                
                if (failures >= kMaxRestartRetries) {
                    MEDIA_LOG_E("AAudio: max restart retries (%d) exceeded, stopping", kMaxRestartRetries);
                    lastError_.store(AAUDIO_ERROR_INTERNAL, std::memory_order_relaxed);
                    callbackState_.store(kStateStopped, std::memory_order_release);

                    // Notify caller that recovery failed
                    RestartCallback failCb;
                    {
                        std::lock_guard<std::mutex> cbLk(callbackMtx_);
                        failCb = restartCallback_;
                    }
                    if (failCb) failCb();
                    break;
                }
                
                int backoffMs = std::min(config::aaudio::kRestartDelayMs << failures, kMaxBackoffMs);
                MEDIA_LOG_W("AAudio: restart failed (%d/%d), backoff %dms", 
                            failures, kMaxRestartRetries, backoffMs);
                std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
                continue;
            }

            consecutiveRestartFailures_.store(0, std::memory_order_relaxed);
            restartCount_.fetch_add(1, std::memory_order_relaxed);
            MEDIA_LOG_I("AAudio output: restart #%u successful", restartCount_.load(std::memory_order_relaxed));

            RestartCallback cb;
            {
                std::lock_guard<std::mutex> lk(callbackMtx_);
                cb = restartCallback_;
            }
            if (cb) cb();
        }
        restartThreadExited_.store(true, std::memory_order_release);
    }

    HOT_FUNCTION
    static aaudio_data_callback_result_t dataCallback(
            AAudioStream* stream, void* userData, void* audioData, int32_t numFrames) {
        auto* self = static_cast<AAudioOutputBridge*>(userData);
        auto* output = static_cast<float*>(audioData);

        if (UNLIKELY(numFrames <= 0 || !output)) {
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        }

        const size_t silenceBytes = static_cast<size_t>(numFrames) * static_cast<size_t>(self->config_.channelCount) * sizeof(float);

        uint8_t state = self->callbackState_.load(std::memory_order_acquire);
        if (UNLIKELY(self->interruptedFlag_ && self->interruptedFlag_->load(std::memory_order_acquire))) {
            std::memset(output, 0, silenceBytes);
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        }
        if (state == kStateWarmUp) {
            // Warm-up mode: output silence to keep hardware hot
            std::memset(output, 0, silenceBytes);
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        }
        if (state != kStateActive) {
            std::memset(output, 0, silenceBytes);
            return AAUDIO_CALLBACK_RESULT_STOP;
        }

        AAudioStream* currentStream = self->stream_.load(std::memory_order_acquire);
        if (currentStream != stream) {
            std::memset(output, 0, silenceBytes);
            return AAUDIO_CALLBACK_RESULT_STOP;
        }

        // Callback jitter: measure deviation from expected interval
        {
            int64_t now = nowUs();
            int64_t prev = self->lastCallbackTimeUs_.load(std::memory_order_relaxed);
            if (prev > 0) {
                int64_t expectedIntervalUs = static_cast<int64_t>(numFrames) * 1000000LL
                    / self->config_.sampleRate;
                int64_t actualInterval = now - prev;
                int64_t deviation = actualInterval > expectedIntervalUs
                    ? actualInterval - expectedIntervalUs
                    : expectedIntervalUs - actualInterval;
                int64_t curPeak = self->callbackJitterUs_.load(std::memory_order_relaxed);
                if (deviation > curPeak) {
                    self->callbackJitterUs_.store(deviation, std::memory_order_relaxed);
                }
            }
            self->lastCallbackTimeUs_.store(now, std::memory_order_relaxed);
        }

        size_t written = 0;
        const size_t requestedFrames = static_cast<size_t>(numFrames);

        if (self->playbackResampler_.isPassthrough()) {
            // No resampling needed — mixer outputs directly to AAudio buffer
            if (self->callback_) {
                written = self->callback_(output, requestedFrames);
            }
        } else {
            // Resampling active (48kHz → hardware rate): compute how many 48kHz
            // frames to request from mixer to produce numFrames at hardware rate.
            // For 48→16 (ratio 3:1): need requestedFrames * 3 source frames.
            int32_t grantedRate = self->grantedSampleRate_.load(std::memory_order_relaxed);
            size_t srcFrames = (requestedFrames * static_cast<size_t>(self->config_.sampleRate)
                               + static_cast<size_t>(grantedRate) - 1)
                               / static_cast<size_t>(grantedRate);
            // Extra source frame absorbs Speex filter delay to prevent
            // silence-pad underruns on Bluetooth SCO (8/16kHz)
            srcFrames += 1;
            srcFrames = std::min(srcFrames, self->resampleSrcBuffer_.size());

            size_t mixerWritten = 0;
            if (self->callback_) {
                mixerWritten = self->callback_(self->resampleSrcBuffer_.data(), srcFrames);
            }

            if (mixerWritten > 0) {
                size_t resampledCount = self->playbackResampler_.process(output, self->resampleSrcBuffer_.data(), mixerWritten);
                // Clamp to AAudio buffer size: the +1 source frame guard can cause
                // the resampler to produce 1 extra output frame (e.g., 48→16kHz:
                // ceil((numFrames*3+1)/3) = numFrames+1). Without clamping, the extra
                // frame overwrites memory past the AAudio-provided buffer.
                written = std::min(resampledCount, requestedFrames);
            }
        }

        if (written > 0) {
            self->framesWritten_.fetch_add(static_cast<int64_t>(written), std::memory_order_relaxed);
        }

        if (written < requestedFrames) {
            size_t remaining = (requestedFrames - written) * static_cast<size_t>(self->config_.channelCount);
            std::memset(output + written * static_cast<size_t>(self->config_.channelCount), 0, remaining * sizeof(float));
        }

        return AAUDIO_CALLBACK_RESULT_CONTINUE;
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

    static void errorCallback(AAudioStream* stream, void* userData, aaudio_result_t error) {
        auto* self = static_cast<AAudioOutputBridge*>(userData);

        uint8_t state = self->callbackState_.load(std::memory_order_acquire);
        if (state != kStateActive && state != kStateWarmUp) return;

        AAudioStream* currentStream = self->stream_.load(std::memory_order_acquire);
        if (currentStream != stream) return;

        // Store error for deferred logging on restart thread (no I/O on RT thread)
        self->lastError_.store(error, std::memory_order_relaxed);

        if (isRecoverableError(error)) {
            self->scheduleRestart();
        } else {
            self->callbackState_.store(kStateStopped, std::memory_order_release);
        }
    }

    const AudioCallback callback_;
    const AAudioConfig config_;

    std::mutex lifecycleMtx_;
    mutable std::mutex streamMtx_;
    std::atomic<AAudioStream*> stream_{nullptr};

    std::thread restartThread_;
    std::atomic<bool> restartRequested_{false};
    std::atomic<bool> restartThreadExited_{false};

    std::mutex callbackMtx_;
    RestartCallback restartCallback_;

    std::atomic<uint8_t> callbackState_{kStateStopped};
    std::atomic<int64_t> framesWritten_{0};
    std::atomic<aaudio_result_t> lastError_{AAUDIO_OK};
    std::atomic<uint32_t> restartCount_{0};
    std::atomic<int> consecutiveRestartFailures_{0};
    std::atomic<LatencyMode> grantedLatencyMode_{LatencyMode::Unknown};
    std::atomic<int32_t> grantedSampleRate_{0};

    std::atomic<bool>* interruptedFlag_{nullptr};

    // Callback jitter tracking (RT-safe: atomics + clock_gettime only)
    std::atomic<int64_t> lastCallbackTimeUs_{0};
    std::atomic<int64_t> callbackJitterUs_{0};      // peak abs deviation from expected interval

    AudioResamplerFloat playbackResampler_;
    // Buffer for mixer output before downsampling.
    // Sized for worst-case: 4096 hw frames at up to 6x upsample ratio (8kHz→48kHz).
    // Matches iOS AVAudioOutputBridge sizing for platform parity.
    static constexpr size_t kResampleSrcCapacity = 4096 * 6;
    alignas(16) std::array<float, kResampleSrcCapacity> resampleSrcBuffer_{};
};

}  // namespace media