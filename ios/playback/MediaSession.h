// Uses AVAudioOutputBridge instead of AAudioOutputBridge
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include "../../cpp/common/MediaConfig.h"
#include "../../cpp/playback/MediaTypes.h"
#include "../../cpp/common/MediaLog.h"
#include "../../cpp/playback/FramePool.h"
#include "../../cpp/playback/JitterEstimator.h"
#include "../../cpp/playback/AudioDecodeChannel.h"
#include "../../cpp/playback/AudioMixer.h"
#include "../../cpp/playback/DecodeThread.h"
#include "../../cpp/common/HealthWatchdog.h"
#include "AVAudioOutputBridge.h"
#include "AudioSessionManager.h"

namespace media {

struct SessionConfig {
    bool enableAudioOutput = true;
    AVAudioConfig audioConfig{};
};

enum class SessionState {
    Stopped = 0,
    Starting,
    Running,
    Stopping
};

struct SessionMetrics {
    uint64_t totalSamplesOutput = 0;
    int64_t uptimeUs = 0;

    bool decodeThreadResponsive = true;
    bool decodeThreadDetached = false;
    bool watchdogTripped = false;
    uint32_t watchdogTripCount = 0;
    int64_t timeSinceHeartbeatUs = 0;

    // Latency mode tracking
    LatencyMode grantedLatencyMode = LatencyMode::Unknown;

    // Audio output health metrics
    uint32_t audioRestartCount = 0;
    int audioLastError = 0;
    int32_t actualSampleRate = 0;
    bool sampleRateValid = true;
    int64_t audioOutputLatencyUs = 0;

    // Pool pressure indicators
    bool decodedPoolUnderPressure = false;
    bool encodedPoolUnderPressure = false;

    // Bluetooth route tracking
    AudioRoute currentRoute = AudioRoute::Unknown;

    // Audio channel metrics
    uint64_t audioFramesReceived = 0;
    uint64_t framesDropped = 0;
    uint64_t framesDrained = 0;
    uint64_t decodeErrors = 0;
    uint64_t decoderResets = 0;
    uint64_t underruns = 0;
    uint64_t ptsDiscontinuities = 0;
    uint64_t plcFrames = 0;
    bool muted = false;

    // Clock drift compensation metrics
    int32_t driftPpm = 0;
    bool driftCompensationActive = false;
    float driftCurrentRatio = 1.0f;
    float catchupCurrentRatio = 1.0f;

    // Bluetooth crackling diagnostics
    uint64_t fastPathSwitches = 0;
    uint64_t catchupDeadZoneSnaps = 0;

    // Inter-frame delivery gap diagnostics
    int64_t maxInterFrameGapUs = 0;
    uint64_t gapsOver50ms = 0;
    uint64_t gapsOver100ms = 0;
    uint64_t gapsOver500ms = 0;

    // Jitter buffer metrics
    int64_t jitterUs = 0;
    int64_t bufferTargetUs = 0;

    // Triage metrics: pipeline state observability
    uint8_t audioStreamState = 0;     // StreamState enum value
    float currentGain = 1.0f;
    int64_t bufferedDurationUs = 0;   // total audio buffered (encoded + decoded)
    int64_t decodedDurationUs = 0;    // decoded audio ready for playback

    // Audio level metering (post-gain, from audio callback)
    float peakLevel = 0.0f;         // Linear 0.0-1.0+ (>1.0 = clipping)
    float rmsLevel = 0.0f;          // Linear RMS
    float peakDbfs = -100.0f;       // dBFS peak
    float rmsDbfs = -100.0f;        // dBFS RMS
    uint64_t clipCount = 0;         // Frames that exceeded 1.0
};

class MediaSession {
public:
    explicit MediaSession(SessionConfig config = {}) noexcept
        : config_(config)
        , decodeThread_(audio_) {

        mixer_.setChannel(&audio_);
    }

    ~MediaSession() noexcept { stop(); }

    MediaSession(const MediaSession&) = delete;
    MediaSession& operator=(const MediaSession&) = delete;

    [[nodiscard]] bool start() noexcept {
        std::lock_guard<std::mutex> lk(lifecycleMtx_);

        SessionState expected = SessionState::Stopped;
        if (!state_.compare_exchange_strong(expected, SessionState::Starting,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return expected == SessionState::Running;
        }

        startTimeUs_ = nowUs();
        MEDIA_LOG_I("MediaSession starting");

        // Initialize AudioSessionManager for route/interruption handling.
        // Skip if warmUp() already called initialize().
        auto& mgr = AudioSessionManager::instance();
        if (!sessionMgr_.load(std::memory_order_acquire)) {
            mgr.initialize();
        }
        sessionMgr_.store(&mgr, std::memory_order_release);

        audio_.setDecoder(std::make_unique<OpusDecoderAdapter>());

        if (!decodeThread_.start()) {
            MEDIA_LOG_E("MediaSession::start failed: decode thread start failed");
            sessionMgr_.store(nullptr, std::memory_order_release);
            AudioSessionManager::instance().shutdown();
            state_.store(SessionState::Stopped, std::memory_order_release);
            return false;
        }

        if (config_.enableAudioOutput) {
            // Check if audio was pre-warmed - if so, just transition to active
            bool audioPreWarmed = audioOutput_ && audioOutput_->isWarmingUp();

            if (!audioOutput_) {
                audioOutput_ = std::make_unique<AVAudioOutputBridge>(
                    [this](float* output, size_t frames) { return onAudioCallback(output, frames); },
                    config_.audioConfig
                );
                audioOutput_->setRestartCallback([this] { decodeThread_.wake(); });
                audioOutput_->setInterruptedFlag(AudioSessionManager::instance().interruptedFlagPtr());
            }

            if (!audioOutput_->start()) {
                // Audio output may fail during a BT route transition.
                // Don't fail the pipeline; rely on route change notification to recover.
                MEDIA_LOG_W("MediaSession::start: audio output failed, will recover on route change");
                audioOutput_->stop();
                audioOutput_.reset();
            }

            if (audioPreWarmed) {
                MEDIA_LOG_I("MediaSession: audio transitioned from warm-up to active (zero latency)");
            }
        }

        state_.store(SessionState::Running, std::memory_order_release);

        // If audio output bridge was destroyed during start, attempt recovery now.
        if (config_.enableAudioOutput && !audioOutput_) {
            MEDIA_LOG_I("MediaSession: audio output null after start, attempting recovery");
            restartAudioOutput();
        }

        MEDIA_LOG_I("MediaSession started successfully");
        return true;
    }

    void stop() noexcept {
        std::lock_guard<std::mutex> lk(lifecycleMtx_);

        SessionState current = state_.load(std::memory_order_acquire);
        if (current == SessionState::Stopped) {
            // warmUp() may have called initialize() without start() ever running.
            if (sessionMgr_.load(std::memory_order_acquire)) {
                sessionMgr_.store(nullptr, std::memory_order_release);
                AudioSessionManager::instance().shutdown();
            }
            return;
        }

        if (!state_.compare_exchange_strong(current, SessionState::Stopping,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return;
        }

        MEDIA_LOG_I("MediaSession stopping");

        if (audioOutput_) {
            audioOutput_->stop();
            audioOutput_.reset();
        }

        decodeThread_.stop();
        audio_.deactivate();
        jitter_.reset();

        sessionMgr_.store(nullptr, std::memory_order_release);
        AudioSessionManager::instance().shutdown();

        state_.store(SessionState::Stopped, std::memory_order_release);
        MEDIA_LOG_I("MediaSession stopped");
    }

    [[nodiscard]] bool isRunning() const noexcept {
        return state_.load(std::memory_order_acquire) == SessionState::Running;
    }

    // Pre-warm the audio hardware to reduce first-frame latency.
    [[nodiscard]] bool warmUp() noexcept {
        std::lock_guard<std::mutex> lk(lifecycleMtx_);

        // Initialize AudioSessionManager if not already done.
        if (!sessionMgr_.load(std::memory_order_acquire)) {
            auto& mgr = AudioSessionManager::instance();
            mgr.initialize();
            sessionMgr_.store(&mgr, std::memory_order_release);
        }

        if (!config_.enableAudioOutput) return true;

        if (audioOutput_ && audioOutput_->isRunning()) {
            return true;
        }

        if (!audioOutput_) {
            audioOutput_ = std::make_unique<AVAudioOutputBridge>(
                [this](float* output, size_t frames) { return onAudioCallback(output, frames); },
                config_.audioConfig
            );
            audioOutput_->setInterruptedFlag(AudioSessionManager::instance().interruptedFlagPtr());
        }

        if (!audioOutput_->warmUp()) {
            MEDIA_LOG_E("MediaSession::warmUp failed: audio output warm-up failed");
            return false;
        }

        MEDIA_LOG_I("MediaSession: audio hardware warm-up complete");
        return true;
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
        (void)videoUs;  // forwarded by caller to VideoFrameQueue
    }

    void setNetworkJitterFloorUs(int64_t us) noexcept {
        jitter_.setNetworkJitterFloorUs(us);
    }

    // Reset jitter/drift state for a new stream without stopping pipelines.
    // Called by resetStream() so stale estimates and frames don't carry over.
    void resetStreamState() noexcept {
        jitter_.reset();
        audio_.requestDriftReset();
        audio_.requestEncodedClear();
    }

    [[nodiscard]] int64_t uptimeUs() const noexcept {
        return startTimeUs_ > 0 ? nowUs() - startTimeUs_ : 0;
    }

    [[nodiscard]] bool pushAudioFrame(const uint8_t* data, size_t size,
                                       int64_t timestampUs, int64_t durationUs = 0) noexcept {
        if (!isRunning()) return false;

        checkAudioOutputHealth();

        // Activate audio on first frame
        if (!audio_.isActive()) {
            audio_.activate();
            decodeThread_.notifyActive();
        }

        jitter_.onPacket(timestampUs, nowUs());
        audio_.setBufferTarget(jitter_.bufferTargetUs());

        // Update clock drift compensation if we have enough measurement data
        if (jitter_.hasValidLongTermDrift()) {
            audio_.updateDriftCompensation(
                jitter_.cumulativeDriftUs(),
                jitter_.measurementWindowUs(),
                jitter_.sampleCount());
        }

        RawAudioFrame frame;
        frame.data = data;
        frame.size = size;
        frame.timestampUs = timestampUs;
        frame.durationUs = durationUs;
        bool pushed = audio_.pushEncodedFrame(frame);

        if (pushed) {
            decodeThread_.wake();
        }

        return pushed;
    }

    void setGain(float gain) noexcept {
        audio_.setGain(gain);
    }

    void setMuted(bool muted) noexcept {
        muted_ = muted;
        audio_.setGain(muted ? 0.0f : config::mix::kDefaultGain);
    }

    [[nodiscard]] bool isMuted() const noexcept { return muted_; }

    void getMetrics(SessionMetrics& out) noexcept {
        out = metrics();
    }

    [[nodiscard]] SessionMetrics metrics() noexcept {
        std::lock_guard<std::mutex> lk(lifecycleMtx_);

        SessionMetrics m{};
        m.uptimeUs = uptimeUs();

        const AVAudioOutputBridge* audio = audioOutput_.get();

        m.totalSamplesOutput = audio ? audio->framesWritten() : 0;

        m.decodeThreadResponsive = decodeThread_.isResponsive();
        m.decodeThreadDetached = decodeThread_.wasDetached();
        m.watchdogTripped = decodeThread_.wasWatchdogTripped();
        m.watchdogTripCount = decodeThread_.watchdogTripCount();
        m.timeSinceHeartbeatUs = decodeThread_.timeSinceLastHeartbeatUs();

        // Latency mode tracking
        m.grantedLatencyMode = audio ? audio->grantedLatencyMode() : LatencyMode::Unknown;

        // Audio channel metrics
        if (audio_.isActive()) {
            const auto& sm = audio_.metrics();
            m.audioFramesReceived = sm.framesReceived.load(std::memory_order_relaxed);
            m.framesDropped = sm.framesDropped.load(std::memory_order_relaxed);
            m.framesDrained = sm.framesDrained.load(std::memory_order_relaxed);
            m.decodeErrors = sm.decodeErrors.load(std::memory_order_relaxed);
            m.decoderResets = sm.decoderResets.load(std::memory_order_relaxed);
            m.underruns = sm.underruns.load(std::memory_order_relaxed);
            m.ptsDiscontinuities = sm.ptsDiscontinuities.load(std::memory_order_relaxed);
            m.plcFrames = sm.plcFrames.load(std::memory_order_relaxed);
            m.muted = muted_;

            // Bluetooth crackling diagnostics
            m.fastPathSwitches = sm.fastPathSwitches.load(std::memory_order_relaxed);
            m.catchupDeadZoneSnaps = audio_.catchupDeadZoneSnaps();

            // Inter-frame delivery gap diagnostics
            m.maxInterFrameGapUs = sm.maxInterFrameGapUs.load(std::memory_order_relaxed);
            m.gapsOver50ms = sm.gapsOver50ms.load(std::memory_order_relaxed);
            m.gapsOver100ms = sm.gapsOver100ms.load(std::memory_order_relaxed);
            m.gapsOver500ms = sm.gapsOver500ms.load(std::memory_order_relaxed);

            // Clock drift compensation metrics
            m.driftPpm = audio_.driftPpm();
            m.driftCompensationActive = audio_.isDriftCompensationActive();
            m.driftCurrentRatio = audio_.driftCurrentRatio();
            m.catchupCurrentRatio = audio_.catchupCurrentRatio();

            // Jitter buffer metrics
            m.jitterUs = jitter_.jitterUs();
            m.bufferTargetUs = jitter_.bufferTargetUs();

            // Check pool pressure
            m.decodedPoolUnderPressure = audio_.isDecodedPoolUnderPressure();
            m.encodedPoolUnderPressure = audio_.isEncodedPoolUnderPressure();

            // Triage metrics
            m.audioStreamState = static_cast<uint8_t>(audio_.state());
            m.currentGain = audio_.targetGain();
            m.bufferedDurationUs = audio_.bufferedDurationUs();
            m.decodedDurationUs = audio_.decodedDurationUs();
        }

        // Audio level metering
        const auto& meter = mixer_.levelMeter();
        m.peakLevel = meter.peakLevel();
        m.rmsLevel = meter.rmsLevel();
        m.peakDbfs = AudioLevelMeter::toDbfs(m.peakLevel);
        m.rmsDbfs = AudioLevelMeter::toDbfs(m.rmsLevel);
        m.clipCount = meter.clipCount();

        // Audio output health metrics
        if (audio) {
            m.audioRestartCount = audio->restartCount();
            m.audioLastError = audio->lastError();
            m.actualSampleRate = audio->actualSampleRate();
            m.sampleRateValid = audio->isSampleRateValid();
            m.audioOutputLatencyUs = audio->totalLatencyUs();
        }

        // Bluetooth route tracking
        m.currentRoute = AudioSessionManager::instance().currentRoute();

        return m;
    }

    void resetMetrics() noexcept {
        audio_.resetMetrics();
        audioPool_.resetExhaustedCount();
        MEDIA_LOG_I("MediaSession: metrics reset");
    }

    void setCatchupPolicy(CatchupPolicy p) noexcept {
        audio_.setCatchupPolicy(p);
    }

    void setPlaybackRate(float rate) noexcept {
        audio_.setPlaybackRate(rate);
    }

    [[nodiscard]] float playbackRate() const noexcept {
        return audio_.playbackRate();
    }

    void setStreamStatus(StreamStatus status) noexcept {
        streamStatus_.store(status, std::memory_order_relaxed);
    }

    [[nodiscard]] StreamStatus streamStatus() const noexcept {
        return streamStatus_.load(std::memory_order_relaxed);
    }

    void setSyncCoordinator(AVSyncCoordinator* coordinator) noexcept {
        audio_.setSyncCoordinator(coordinator);
    }

    [[nodiscard]] bool isDecodeThreadResponsive() noexcept {
        return decodeThread_.isResponsive();
    }

    [[nodiscard]] bool wasWatchdogTripped() const noexcept {
        return decodeThread_.wasWatchdogTripped();
    }

    [[nodiscard]] int64_t timeSinceLastHeartbeatUs() const noexcept {
        return decodeThread_.timeSinceLastHeartbeatUs();
    }

    void resetWatchdogState() noexcept {
        decodeThread_.resetWatchdogState();
    }

    void requestDriftReset() noexcept {
        audio_.requestDriftReset();
    }

    void setHealthWatchdog(std::unique_ptr<HealthWatchdog> wd) noexcept {
        decodeThread_.setHealthWatchdog(std::move(wd));
    }

    // Lock-free accessors for health watchdog (called from decode thread)
    [[nodiscard]] const auto& audioMetrics() const noexcept { return audio_.metrics(); }
    [[nodiscard]] bool wasDecodeThreadDetached() const noexcept { return decodeThread_.wasDetached(); }
    [[nodiscard]] bool isAudioOutputRunning() const noexcept { return audioOutput_ && audioOutput_->isRunning(); }
    [[nodiscard]] int64_t bufferTargetUs() const noexcept { return audio_.bufferTarget(); }
    [[nodiscard]] int64_t bufferedDurationUs() const noexcept { return audio_.bufferedDurationUs(); }
    [[nodiscard]] int64_t decodedDurationUs() const noexcept { return audio_.decodedDurationUs(); }

private:
    // Periodic liveness check: if session is Running but audio output is null
    // or stopped, attempt recovery.
    void checkAudioOutputHealth() noexcept {
        if (!config_.enableAudioOutput) return;

        int64_t now = nowUs();
        if (now - lastAudioHealthCheckUs_ < config::thread::kAudioOutputCheckIntervalUs) return;
        lastAudioHealthCheckUs_ = now;

        if (!audioOutput_ || !audioOutput_->isRunning()) {
            std::lock_guard<std::mutex> lk(lifecycleMtx_);
            if (state_.load(std::memory_order_acquire) != SessionState::Running) return;
            if (audioOutput_ && audioOutput_->isRunning()) return;

            MEDIA_LOG_W("MediaSession: audio output not running, attempting recovery");
            restartAudioOutput();
        }
    }

    void restartAudioOutput() noexcept {
        if (!config_.enableAudioOutput) return;

        if (!audioOutput_) {
            audioOutput_ = std::make_unique<AVAudioOutputBridge>(
                [this](float* out, size_t frames) { return onAudioCallback(out, frames); },
                config_.audioConfig);
            audioOutput_->setRestartCallback([this] { decodeThread_.wake(); });
            audioOutput_->setInterruptedFlag(AudioSessionManager::instance().interruptedFlagPtr());
        }

        if (!audioOutput_->start()) {
            MEDIA_LOG_E("MediaSession: failed to restart audio output");
        }
    }

    size_t onAudioCallback(float* output, size_t frameCount) noexcept {
        if (state_.load(std::memory_order_acquire) != SessionState::Running) {
            return 0;
        }

        return mixer_.mix(output, frameCount);
    }

    const SessionConfig config_;
    std::mutex lifecycleMtx_;
    std::atomic<SessionState> state_{SessionState::Stopped};
    int64_t startTimeUs_ = 0;

    DecodedAudioPool audioPool_;
    AudioDecodeChannel audio_{audioPool_};
    JitterEstimator jitter_;

    AudioMixer mixer_;
    DecodeThread decodeThread_;

    std::unique_ptr<AVAudioOutputBridge> audioOutput_;

    // Cached AudioSessionManager pointer for RT callback path (avoids Meyers-singleton guard)
    std::atomic<AudioSessionManager*> sessionMgr_{nullptr};

    bool muted_ = false;
    std::atomic<StreamStatus> streamStatus_{StreamStatus::Live};

    // Audio output liveness check — JS thread only, no synchronization needed.
    int64_t lastAudioHealthCheckUs_ = 0;
};

}  // namespace media
