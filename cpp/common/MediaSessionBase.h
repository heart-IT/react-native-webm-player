// Shared media session template: owns the audio decode channel, platform audio
// output bridge, audio session manager, and decode thread.
// Parameterized by PlatformTraits which provides:
//   - Bridge: audio output bridge type (AAudioOutputBridge / AVAudioOutputBridge)
//   - BridgeConfig: bridge configuration type (AAudioConfig / AudioOutputConfig)
//   - shutdownManager(): platform-specific AudioSessionManager shutdown
//
// This eliminates ~650 lines of duplication between Android and iOS MediaSession.h.
// Platform parity is enforced by the compiler — not by convention or auditing.
//
// Implementation is split across sibling files (all included at the bottom):
//   SessionMetrics.h                — metrics output struct
//   MediaSessionLifecycle.inl       — start/stop/warmUp + private restart helpers
//   MediaSessionMetricsImpl.inl     — metrics() snapshot assembly
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include "AudioRouteTypes.h"
// std::weak_ptr<void> for owner liveness gating in the
// AudioSessionManager::restartCallback wired in MediaSessionLifecycle.inl.
#include "MediaConfig.h"
#include "MediaLog.h"
#include "MediaTime.h"
#include "HealthWatchdog.h"
#include "IngestRingBuffer.h"
#include "SessionMetrics.h"
#include "../playback/MediaTypes.h"
#include "../playback/FramePool.h"
#include "../playback/JitterEstimator.h"
#include "../playback/AudioDecodeChannel.h"
#include "../playback/AudioMixer.h"
#include "../playback/DecodeThread.h"
#include "../transcript/TranscriptRingBuffer.h"

namespace media {

class AudioSessionManager;  // Platform-specific, included via PlatformTraits header

enum class SessionState {
    Stopped = 0,
    Starting,
    Running,
    Stopping
};

// PlatformTraits must provide:
//   using Bridge = <AudioOutputBridge type>;
//   using BridgeConfig = <bridge config type>;
//   static void shutdownManager();
//   static AudioSessionManager& managerInstance();

template<typename PlatformTraits>
class MediaSessionBase {
    using Bridge = typename PlatformTraits::Bridge;
    using BridgeConfig = typename PlatformTraits::BridgeConfig;

public:
    struct Config {
        bool enableAudioOutput = true;
        BridgeConfig audioConfig{};
    };

    explicit MediaSessionBase(Config config = {}) noexcept
        : config_(config)
        , decodeThread_(audio_) {
        mixer_.setChannel(&audio_);
    }

    ~MediaSessionBase() noexcept { stop(); }

    MediaSessionBase(const MediaSessionBase&) = delete;
    MediaSessionBase& operator=(const MediaSessionBase&) = delete;

    // Lifecycle (defined in MediaSessionLifecycle.inl)
    [[nodiscard]] bool start() noexcept;
    void stop() noexcept;
    [[nodiscard]] bool warmUp() noexcept;

    [[nodiscard]] bool isRunning() const noexcept {
        return state_.load(std::memory_order_acquire) == SessionState::Running;
    }

    [[nodiscard]] bool isWarmingUp() const noexcept {
        return audioOutput_ && audioOutput_->isWarmingUp();
    }

    [[nodiscard]] bool pause() noexcept {
        if (!isRunning()) return false;
        audio_.pause();
        if (audioOutput_) audioOutput_->pauseToWarmUp();
        return true;
    }

    [[nodiscard]] bool resume() noexcept {
        if (!isRunning()) return false;
        audio_.resume();
        if (audioOutput_) audioOutput_->resumeFromWarmUp();
        decodeThread_.wake();
        return true;
    }

    [[nodiscard]] bool isPaused() const noexcept {
        return audio_.state() == StreamState::Paused;
    }

    [[nodiscard]] int64_t currentTimeUs() const noexcept {
        return audio_.lastRenderedPtsUs();
    }

    [[nodiscard]] StreamState audioState() const noexcept {
        return audio_.state();
    }

    void setBufferTargetOverride(int64_t audioUs, int64_t videoUs) noexcept {
        jitter_.setBufferTargetOverride(audioUs);
        (void)videoUs;
    }

    void setNetworkJitterFloorUs(int64_t us) noexcept {
        jitter_.setNetworkJitterFloorUs(us);
    }

    void resetStreamState() noexcept {
        jitter_.reset();
        audio_.requestDriftReset();
        audio_.requestEncodedClear();
        audio_.resetOffsetTracking();
    }

    void cancelPendingEncodedClear() noexcept {
        audio_.cancelPendingEncodedClear();
    }

    [[nodiscard]] int64_t uptimeUs() const noexcept {
        return startTimeUs_ > 0 ? nowUs() - startTimeUs_ : 0;
    }

    [[nodiscard]] bool pushAudioFrame(long long absOffset, size_t size,
                                       int64_t timestampUs, int64_t durationUs = 0) noexcept {
        if (!isRunning()) return false;

        checkAudioOutputHealth();

        if (!audio_.isActive()) {
            audio_.activate();
            decodeThread_.notifyActive();
        }

        jitter_.onSample(timestampUs, nowUs());
        audio_.setBufferTarget(jitter_.bufferTargetUs());
        audio_.setArrivalConfidence(jitter_.arrivalConfidence());

        if (jitter_.hasValidLongTermDrift()) {
            audio_.updateDriftCompensation(
                jitter_.cumulativeDriftUs(),
                jitter_.measurementWindowUs(),
                jitter_.sampleCount());
        }

        RawAudioFrame frame;
        frame.absOffset = absOffset;
        frame.size = size;
        frame.timestampUs = timestampUs;
        frame.durationUs = durationUs;
        bool pushed = audio_.pushEncodedFrame(frame);

        if (pushed) {
            decodeThread_.wake();
        }

        return pushed;
    }

    void setDecodeRingBuffer(IngestRingBuffer* ring) noexcept {
        audio_.setRingBuffer(ring);
    }

    void waitForDecodeIdle() noexcept {
        audio_.waitForDecodeIdle();
    }

    [[nodiscard]] long long lastDecodedAbsOffset() const noexcept {
        return audio_.lastDecodedAbsOffset();
    }

    [[nodiscard]] long long retentionFloorAbsOffset() const noexcept {
        return audio_.retentionFloorAbsOffset();
    }

    void setGain(float gain) noexcept { audio_.setGain(gain); }

    void setMuted(bool muted) noexcept {
        muted_ = muted;
        audio_.setGain(muted ? 0.0f : config::mix::kDefaultGain);
    }

    [[nodiscard]] bool isMuted() const noexcept { return muted_; }

    void getMetrics(SessionMetrics& out) noexcept { out = metrics(); }

    // Defined in MediaSessionMetricsImpl.inl
    [[nodiscard]] SessionMetrics metrics() noexcept;

    void setCatchupPolicy(CatchupPolicy p) noexcept { audio_.setCatchupPolicy(p); }
    void setPlaybackRate(float rate) noexcept { audio_.setPlaybackRate(rate); }
    [[nodiscard]] float playbackRate() const noexcept { return audio_.playbackRate(); }

    void setStreamStatus(StreamStatus status) noexcept {
        streamStatus_.store(status, std::memory_order_relaxed);
    }
    [[nodiscard]] StreamStatus streamStatus() const noexcept {
        return streamStatus_.load(std::memory_order_relaxed);
    }

    void setSyncCoordinator(AVSyncCoordinator* coordinator) noexcept {
        audio_.setSyncCoordinator(coordinator);
    }

    // Liveness handle for the AudioSessionManager session-restart callback
    // (iOS interruption-end / Android focus-regain / media-services-reset).
    // The callback locks this weak_ptr first; if the owning module is gone,
    // the callback exits before touching `this`. Set once at module init.
    void setOwnerWeak(std::weak_ptr<void> w) noexcept { weakOwner_ = std::move(w); }

    void setTranscriptBuffer(transcript::TranscriptRingBuffer* buf) noexcept {
        transcriptBuffer_.store(buf, std::memory_order_release);
    }

    void waitForTranscriptDrain() noexcept {
        while (transcriptInflight_.load(std::memory_order_acquire) > 0) {
            std::this_thread::yield();
        }
    }

    [[nodiscard]] bool isDecodeThreadResponsive() noexcept { return decodeThread_.isResponsive(); }
    [[nodiscard]] bool wasWatchdogTripped() const noexcept { return decodeThread_.wasWatchdogTripped(); }
    [[nodiscard]] int64_t timeSinceLastHeartbeatUs() const noexcept { return decodeThread_.timeSinceLastHeartbeatUs(); }
    void resetWatchdogState() noexcept { decodeThread_.resetWatchdogState(); }
    void requestDriftReset() noexcept { audio_.requestDriftReset(); }

    void setHealthWatchdog(std::unique_ptr<HealthWatchdog> wd) noexcept {
        decodeThread_.setHealthWatchdog(std::move(wd));
    }

    void resetHealthWindow() noexcept { decodeThread_.resetHealthWindow(); }

    // Lock-free accessors for health watchdog (called from decode thread)
    [[nodiscard]] const auto& audioMetrics() const noexcept { return audio_.metrics(); }
    [[nodiscard]] bool wasDecodeThreadDetached() const noexcept { return decodeThread_.wasDetached(); }
    [[nodiscard]] bool isAudioOutputRunning() const noexcept { return audioOutput_ && audioOutput_->isRunning(); }
    [[nodiscard]] bool isDecodedPoolUnderPressure() const noexcept { return audio_.isDecodedPoolUnderPressure(); }
    [[nodiscard]] bool isEncodedPoolUnderPressure() const noexcept { return audio_.isEncodedPoolUnderPressure(); }
    [[nodiscard]] int64_t bufferTargetUs() const noexcept { return audio_.bufferTarget(); }
    [[nodiscard]] float arrivalConfidence() const noexcept { return jitter_.arrivalConfidence(); }
    [[nodiscard]] int64_t bufferedDurationUs() const noexcept { return audio_.bufferedDurationUs(); }
    [[nodiscard]] int64_t decodedDurationUs() const noexcept { return audio_.decodedDurationUs(); }

private:
    // Defined in MediaSessionLifecycle.inl
    void checkAudioOutputHealth() noexcept;
    void restartAudioOutput() noexcept;

    size_t onAudioCallback(float* output, size_t frameCount) noexcept {
        if (state_.load(std::memory_order_acquire) != SessionState::Running) {
            return 0;
        }

        size_t written = mixer_.mix(output, frameCount);
        // Increment BEFORE loading the buffer pointer so the drain on JS thread
        // (setTranscriptBuffer(nullptr) → waitForTranscriptDrain) cannot observe
        // zero inflight while a callback is about to push into a dead buffer.
        transcriptInflight_.fetch_add(1, std::memory_order_acq_rel);
        if (auto* buf = transcriptBuffer_.load(std::memory_order_acquire)) {
            buf->push(output, written);
        }
        transcriptInflight_.fetch_sub(1, std::memory_order_release);
        return written;
    }

    const Config config_;
    std::mutex lifecycleMtx_;
    std::atomic<SessionState> state_{SessionState::Stopped};
    std::atomic<transcript::TranscriptRingBuffer*> transcriptBuffer_{nullptr};
    std::atomic<int32_t> transcriptInflight_{0};
    int64_t startTimeUs_ = 0;

    DecodedAudioPool audioPool_;
    AudioDecodeChannel audio_{audioPool_};
    JitterEstimator jitter_;

    AudioMixer mixer_;
    DecodeThread decodeThread_;

    std::unique_ptr<Bridge> audioOutput_;

    // Cached AudioSessionManager pointer for RT-safe access (avoids Meyers-singleton guard)
    std::atomic<AudioSessionManager*> sessionMgr_{nullptr};

    bool muted_ = false;
    std::atomic<StreamStatus> streamStatus_{StreamStatus::Live};

    int64_t lastAudioHealthCheckUs_ = 0;

    std::weak_ptr<void> weakOwner_;
};

}  // namespace media

// Out-of-class template-method definitions. Included at the bottom so the
// MediaSessionBase class is fully visible inside each .inl.
#include "MediaSessionLifecycle.inl"
#include "MediaSessionMetricsImpl.inl"
