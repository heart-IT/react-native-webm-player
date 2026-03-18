// Uses AVAudioOutputBridge instead of AAudioOutputBridge
#pragma once

#include <memory>
#include <mutex>
#include <array>
#include <string>
#include "../../cpp/common/MediaConfig.h"
#include "../../cpp/playback/MediaTypes.h"
#include "../../cpp/common/MediaLog.h"
#include "../../cpp/playback/FramePool.h"
#include "../../cpp/playback/SyncController.h"
#include "../../cpp/playback/AudioDecodeSlot.h"
#include "../../cpp/playback/AudioMixer.h"
#include "../../cpp/playback/Participant.h"
#include "../../cpp/playback/SlotAdmission.h"
#include "../../cpp/playback/DecodeThread.h"
#include "AVAudioOutputBridge.h"
#include "AudioSessionManager.h"

namespace media {

struct SessionConfig {
    int localParticipantId = config::sentinel::kNoParticipant;
    bool enableAudioOutput = true;
    AVAudioConfig audioConfig{};
};

enum class SessionState {
    Stopped = 0,
    Starting,
    Running,
    Stopping
};

struct ParticipantMetrics {
    bool active = false;
    int audioSlot = -1;
    uint64_t audioFramesReceived = 0;
    uint64_t framesDropped = 0;
    uint64_t framesDrained = 0;
    uint64_t decodeErrors = 0;
    uint64_t decoderResets = 0;
    uint64_t underruns = 0;
    uint64_t ptsDiscontinuities = 0;
    bool muted = false;

    // Clock drift compensation metrics
    int32_t driftPpm = 0;              // Current drift estimate in parts-per-million
    bool driftCompensationActive = false;

    // Bluetooth crackling diagnostics
    uint64_t fastPathSwitches = 0;
    uint64_t catchupDeadZoneSnaps = 0;

    // Inter-frame delivery gap diagnostics
    int64_t maxInterFrameGapUs = 0;
    uint64_t gapsOver50ms = 0;
    uint64_t gapsOver100ms = 0;
    uint64_t gapsOver500ms = 0;

    // Per-participant jitter buffer metrics (from JitterEstimator)
    int64_t jitterUs = 0;
    int64_t bufferTargetUs = 0;
    float networkStability = 1.0f;
};

struct SessionMetrics {
    int activeParticipants = 0;
    int boundAudioSlots = 0;
    uint64_t totalSamplesOutput = 0;
    int64_t uptimeUs = 0;

    bool decodeThreadResponsive = true;
    bool decodeThreadDetached = false;
    bool watchdogTripped = false;
    uint32_t watchdogTripCount = 0;
    int64_t timeSinceHeartbeatUs = 0;

    // Latency mode tracking
    LatencyMode grantedLatencyMode = LatencyMode::Unknown;

    // Audio output health (P2: expose restart metrics to JS)
    uint32_t audioRestartCount = 0;
    int audioLastError = 0;
    int32_t actualSampleRate = 0;
    bool sampleRateValid = true;
    int64_t audioOutputLatencyUs = 0;

    // Pool pressure indicators
    bool decodedPoolUnderPressure = false;
    bool encodedPoolUnderPressure = false;

    // Bluetooth route diagnostics (codec info not available on iOS)
    AudioRoute currentRoute = AudioRoute::Unknown;
    int64_t routeLatencyUs = 0;

    // Mixer-level diagnostics
    uint64_t softClipClamps = 0;

    std::array<ParticipantMetrics, config::limits::kMaxParticipants> participants;
};

class MediaSession {
public:
    static constexpr int kMaxAudioSlots = config::limits::kMaxAudioSlots;

    explicit MediaSession(SessionConfig config = {}) noexcept
        : config_(config)
        , decodeThread_(audioSlots_)
        , admission_(participants_, audioSlots_, sync_, decodeThread_) {

        for (size_t i = 0; i < static_cast<size_t>(kMaxAudioSlots); ++i) {
            auto slot = std::make_unique<AudioDecodeSlot>(audioPool_);
            slot->setSlotId(static_cast<int>(i));
            audioSlotStorage_[i] = std::move(slot);
            audioSlots_[i] = audioSlotStorage_[i].get();
        }

        mixer_.setSlots(audioSlots_);

        decodeThread_.setInactivityCallback(participants_,
            [](void* ctx, int slotIndex) {
                static_cast<SlotAdmission*>(ctx)->releaseInactiveSlot(slotIndex);
            }, &admission_);
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
        // Cache reference for RT callback path (avoids Meyers-singleton guard lock).
        // Skip if warmUp() already called initialize() — each initialize() bumps
        // refCount, and we only call shutdown() once in stop().
        auto& mgr = AudioSessionManager::instance();
        if (!sessionMgr_.load(std::memory_order_acquire)) {
            mgr.initialize();
        }
        sessionMgr_.store(&mgr, std::memory_order_release);

        if (!mgr.canPlayAudio()) {
            MEDIA_LOG_W("MediaSession::start - audio may be interrupted");
        }

        for (size_t i = 0; i < static_cast<size_t>(kMaxAudioSlots); ++i) {
            if (audioSlots_[i]) {
                audioSlots_[i]->setDecoder(std::make_unique<OpusDecoderAdapter>());
            }
        }

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
            }

            if (!audioOutput_->start()) {
                // Audio output may fail during a BT SCO route transition — iOS
                // negotiates the HFP link asynchronously after setCategory: returns.
                // Don't fail the pipeline; destroy the bridge and rely on the route
                // change notification to call restartAudioOutput() which creates a
                // fresh bridge once BT has settled at the correct sample rate.
                // We cannot use scheduleRestart() here because the bridge's restart
                // thread only exists after a successful start().
                MEDIA_LOG_W("MediaSession::start: audio output failed, will recover on route change");
                audioOutput_->stop();
                audioOutput_.reset();
            }

            if (audioPreWarmed) {
                MEDIA_LOG_I("MediaSession: audio transitioned from warm-up to active (zero latency)");
            }
        }

        state_.store(SessionState::Running, std::memory_order_release);

        // If audio output bridge was destroyed during start (BT transition failure),
        // and a route notification arrived while state was Starting (notification
        // was lost because isRunning() was false), attempt recovery now.
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
            // Clean up the leaked refCount so AudioSessionManager can fully shut down.
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

        for (size_t i = 0; i < static_cast<size_t>(kMaxAudioSlots); ++i) {
            if (audioSlots_[i]) {
                audioSlots_[i]->unbind();
            }
        }

        participants_.reset();
        sync_.resetAll();

        // Shutdown AudioSessionManager to unregister notification observers.
        // Clear cached pointer so next start()/warmUp() calls initialize() again.
        sessionMgr_.store(nullptr, std::memory_order_release);
        AudioSessionManager::instance().shutdown();

        state_.store(SessionState::Stopped, std::memory_order_release);
        MEDIA_LOG_I("MediaSession stopped");
    }

    [[nodiscard]] bool isRunning() const noexcept {
        return state_.load(std::memory_order_acquire) == SessionState::Running;
    }

    // Pre-warm the audio hardware to reduce first-frame latency.
    // Call before audio is expected, then call start() when ready.
    // Safe to call multiple times or before start() - idempotent.
    [[nodiscard]] bool warmUp() noexcept {
        std::lock_guard<std::mutex> lk(lifecycleMtx_);

        // Initialize AudioSessionManager for route handling if not already.
        // Only call initialize() once per lifecycle — each call bumps refCount,
        // and we only call shutdown() once in stop().
        if (!sessionMgr_.load(std::memory_order_acquire)) {
            auto& mgr = AudioSessionManager::instance();
            mgr.initialize();
            sessionMgr_.store(&mgr, std::memory_order_release);
        }

        if (!config_.enableAudioOutput) return true;

        // If already running or warming up, success
        if (audioOutput_ && audioOutput_->isRunning()) {
            return true;
        }

        // Create audio output if needed
        if (!audioOutput_) {
            audioOutput_ = std::make_unique<AVAudioOutputBridge>(
                [this](float* output, size_t frames) { return onAudioCallback(output, frames); },
                config_.audioConfig
            );
        }

        // Warm up the hardware (activates AVAudioSession, starts engine with silence)
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

    [[nodiscard]] int64_t uptimeUs() const noexcept {
        return startTimeUs_ > 0 ? nowUs() - startTimeUs_ : 0;
    }

    [[nodiscard]] Participant* addParticipant(bool isSelf = false) noexcept {
        Participant* p = participants_.acquireSlot();
        if (!p) {
            MEDIA_LOG_W("MediaSession::addParticipant: no free slots");
            return nullptr;
        }

        p->setIsSelf(isSelf);
        p->setState(ParticipantState::Connected);

        MEDIA_LOG_I("MediaSession: added participant %d%s", p->id(), isSelf ? " (self)" : "");
        return p;
    }

    [[nodiscard]] Participant* addParticipantWithId(int externalId) noexcept {
        Participant* p = participants_.findOrCreate(externalId);
        if (!p) {
            MEDIA_LOG_W("MediaSession::addParticipantWithId: no free slots for externalId=%d", externalId);
            return nullptr;
        }

        p->setState(ParticipantState::Connected);

        MEDIA_LOG_I("MediaSession: added participant %d (external=%d)", p->id(), externalId);
        return p;
    }

    void removeParticipant(int participantId) noexcept {
        admission_.releaseParticipant(participantId);
        sync_.release(participantId);
        participants_.remove(participantId);

        MEDIA_LOG_I("MediaSession: removed participant %d", participantId);
    }

    [[nodiscard]] Participant* participant(int id) noexcept {
        return participants_.get(id);
    }

    [[nodiscard]] const Participant* participant(int id) const noexcept {
        return participants_.get(id);
    }

    [[nodiscard]] bool isParticipantActive(int participantId) const noexcept {
        return participants_.get(participantId) != nullptr;
    }

    [[nodiscard]] int participantCount() const noexcept {
        return participants_.count();
    }

    [[nodiscard]] bool pushAudioFrame(int participantId, const uint8_t* data, size_t size,
                                       int64_t timestampUs, int64_t durationUs = 0) noexcept {
        if (!isRunning()) return false;

        checkAudioOutputHealth();

        Participant* p = participants_.get(participantId);
        if (!p) return false;

        if (!admission_.admitParticipant(participantId)) {
            return false;
        }

        int slotIdx = p->audioSlot();
        if (slotIdx < 0 || slotIdx >= kMaxAudioSlots) {
            return false;
        }
        AudioDecodeSlot* slot = audioSlots_[static_cast<size_t>(slotIdx)];
        if (!slot) return false;

        if (auto* jitter = sync_.getJitter(participantId)) {
            jitter->onPacket(timestampUs, nowUs());
            // Apply route-specific latency compensation (e.g., Bluetooth A2DP needs extra buffer)
            int64_t routeOffset = routeLatencyOffsetUs();
            slot->setBufferTarget(jitter->bufferTargetUs(routeOffset));

            // Update clock drift compensation if we have enough measurement data
            if (jitter->hasValidLongTermDrift()) {
                slot->updateDriftCompensation(
                    jitter->cumulativeDriftUs(),
                    jitter->measurementWindowUs(),
                    jitter->sampleCount(),
                    jitter->networkStability());
            }
        }

        RawAudioFrame frame;
        frame.data = data;
        frame.size = size;
        frame.timestampUs = timestampUs;
        frame.durationUs = durationUs;
        bool pushed = slot->pushEncodedFrame(frame);

        if (pushed) {
            p->updateActivity();
            p->setHasAudio(true);
            decodeThread_.wake();
        }

        return pushed;
    }

    [[nodiscard]] bool setParticipantGain(int participantId, float gain) noexcept {
        int slotIndex = audioSlotFor(participantId);
        if (slotIndex >= 0 && slotIndex < kMaxAudioSlots && audioSlots_[slotIndex]) {
            audioSlots_[slotIndex]->setGain(gain);
            return true;
        }
        return false;
    }

    [[nodiscard]] bool setParticipantMuted(int participantId, bool muted) noexcept {
        if (Participant* p = participants_.get(participantId)) {
            p->setMuted(muted);
            (void)setParticipantGain(participantId, muted ? 0.0f : config::mix::kDefaultGain);
            return true;
        }
        return false;
    }

    void scheduleAudioRestart() noexcept {
        std::lock_guard<std::mutex> lk(lifecycleMtx_);
        if (state_.load(std::memory_order_acquire) != SessionState::Running) return;

        if (audioOutput_ && audioOutput_->isRunning()) {
            audioOutput_->scheduleRestart();
        } else {
            restartAudioOutput();
        }
    }

    void pauseAudio() noexcept {
        std::lock_guard<std::mutex> lk(lifecycleMtx_);
        if (audioPaused_.load(std::memory_order_acquire)) return;
        audioPaused_.store(true, std::memory_order_release);
        if (audioOutput_) {
            audioOutput_->stop();
            MEDIA_LOG_I("MediaSession: audio paused");
        }
    }

    void resumeAudio() noexcept {
        std::lock_guard<std::mutex> lk(lifecycleMtx_);
        if (!audioPaused_.load(std::memory_order_acquire)) return;
        audioPaused_.store(false, std::memory_order_release);
        if (state_.load(std::memory_order_acquire) == SessionState::Running) {
            restartAudioOutput();
            MEDIA_LOG_I("MediaSession: audio resumed");
        }
    }

    [[nodiscard]] bool isAudioPaused() const noexcept {
        return audioPaused_.load(std::memory_order_acquire);
    }

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

        // Pool pressure - check any active slot
        bool decodedPressure = false;
        bool encodedPressure = false;

        int activeCount = 0;
        int boundSlots = 0;

        participants_.forEach([&](const Participant& p) {
            if (p.id() >= 0 && p.id() < config::limits::kMaxParticipants) {
                auto& pm = m.participants[static_cast<size_t>(p.id())];
                pm.active = p.isConnected();
                pm.audioSlot = p.audioSlot();
                pm.muted = p.isMuted();

                if (pm.audioSlot >= 0 && pm.audioSlot < kMaxAudioSlots) {
                    if (const auto* slot = audioSlots_[pm.audioSlot]) {
                        const auto& sm = slot->metrics();
                        pm.audioFramesReceived = sm.framesReceived.load(std::memory_order_relaxed);
                        pm.framesDropped = sm.framesDropped.load(std::memory_order_relaxed);
                        pm.framesDrained = sm.framesDrained.load(std::memory_order_relaxed);
                        pm.decodeErrors = sm.decodeErrors.load(std::memory_order_relaxed);
                        pm.decoderResets = sm.decoderResets.load(std::memory_order_relaxed);
                        pm.underruns = sm.underruns.load(std::memory_order_relaxed);
                        pm.ptsDiscontinuities = sm.ptsDiscontinuities.load(std::memory_order_relaxed);

                        // Bluetooth crackling diagnostics
                        pm.fastPathSwitches = sm.fastPathSwitches.load(std::memory_order_relaxed);
                        pm.catchupDeadZoneSnaps = slot->catchupDeadZoneSnaps();

                        // Inter-frame delivery gap diagnostics
                        pm.maxInterFrameGapUs = sm.maxInterFrameGapUs.load(std::memory_order_relaxed);
                        pm.gapsOver50ms = sm.gapsOver50ms.load(std::memory_order_relaxed);
                        pm.gapsOver100ms = sm.gapsOver100ms.load(std::memory_order_relaxed);
                        pm.gapsOver500ms = sm.gapsOver500ms.load(std::memory_order_relaxed);

                        // Clock drift compensation metrics
                        pm.driftPpm = slot->driftPpm();
                        pm.driftCompensationActive = slot->isDriftCompensationActive();

                        // Per-participant jitter buffer metrics
                        if (auto* jitter = sync_.getJitter(p.id())) {
                            pm.jitterUs = jitter->jitterUs();
                            int64_t routeOffset = routeLatencyOffsetUs();
                            pm.bufferTargetUs = jitter->bufferTargetUs(routeOffset);
                            pm.networkStability = jitter->networkStability();
                        }

                        // Check pool pressure on any bound slot
                        if (slot->isDecodedPoolUnderPressure()) decodedPressure = true;
                        if (slot->isEncodedPoolUnderPressure()) encodedPressure = true;
                    }
                    boundSlots++;
                }

                if (pm.active) activeCount++;
            }
        });

        m.activeParticipants = activeCount;
        m.boundAudioSlots = boundSlots;
        m.decodedPoolUnderPressure = decodedPressure;
        m.encodedPoolUnderPressure = encodedPressure;
        m.softClipClamps = mixer_.softClipClamps();

        // Audio output health metrics (P2: expose restart metrics to JS)
        if (audio) {
            m.audioRestartCount = audio->restartCount();
            m.audioLastError = audio->lastError();
            m.actualSampleRate = audio->actualSampleRate();
            m.sampleRateValid = audio->isSampleRateValid();
            m.audioOutputLatencyUs = audio->totalLatencyUs();
        }

        // Bluetooth route diagnostics
        m.currentRoute = AudioSessionManager::instance().currentRoute();
        m.routeLatencyUs = routeLatencyOffsetUs();

        return m;
    }

    void resetMetrics() noexcept {
        for (size_t i = 0; i < static_cast<size_t>(kMaxAudioSlots); ++i) {
            if (audioSlots_[i]) {
                audioSlots_[i]->resetMetrics();
            }
        }
        audioPool_.resetExhaustedCount();
        mixer_.resetSoftClipClamps();
        MEDIA_LOG_I("MediaSession: metrics reset");
    }

    [[nodiscard]] int maxParticipants() const noexcept {
        return config::limits::kMaxParticipants;
    }
    [[nodiscard]] int maxAudioSlots() const noexcept { return kMaxAudioSlots; }

    [[nodiscard]] int audioSlotFor(int participantId) const noexcept {
        return admission_.audioSlotFor(participantId);
    }

    [[nodiscard]] AudioDecodeSlot* audioSlot(int index) noexcept {
        return (index >= 0 && index < kMaxAudioSlots) ? audioSlots_[index] : nullptr;
    }

    void setSyncCoordinator(AVSyncCoordinator* coordinator) noexcept {
        for (int i = 0; i < kMaxAudioSlots; ++i) {
            if (audioSlots_[i]) audioSlots_[i]->setSyncCoordinator(coordinator);
        }
    }

    SyncController& syncController() noexcept { return sync_; }
    ParticipantRegistry& participants() noexcept { return participants_; }

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

    void setEvictionCallback(EvictionCallback cb) noexcept {
        admission_.setEvictionCallback(std::move(cb));
    }

    // Request drift state reset on all bound slots after audio route change.
    // Prevents stale drift ratio from causing wrong playback speed.
    void requestDriftResetAll() noexcept {
        for (size_t i = 0; i < static_cast<size_t>(kMaxAudioSlots); ++i) {
            if (audioSlots_[i] && audioSlots_[i]->isBound()) {
                audioSlots_[i]->requestDriftReset();
            }
        }
    }

private:
    // Periodic liveness check: if session is Running but audio output is null
    // or stopped, attempt recovery. Prevents silent no-audio state when route
    // change notifications are lost (e.g., BT SCO negotiation completes without
    // iOS sending a route change). Time-gated to avoid overhead on every frame push.
    void checkAudioOutputHealth() noexcept {
        if (!config_.enableAudioOutput) return;
        if (audioPaused_.load(std::memory_order_acquire)) return;

        int64_t now = nowUs();
        if (now - lastAudioHealthCheckUs_ < config::thread::kAudioOutputCheckIntervalUs) return;
        lastAudioHealthCheckUs_ = now;

        if (!audioOutput_ || !audioOutput_->isRunning()) {
            std::lock_guard<std::mutex> lk(lifecycleMtx_);
            if (state_.load(std::memory_order_acquire) != SessionState::Running) return;
            if (audioPaused_.load(std::memory_order_acquire)) return;
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
        }

        if (!audioOutput_->start()) {
            MEDIA_LOG_E("MediaSession: failed to restart audio output");
        }
    }

    // Ducking gain applied when audio is interrupted with option to duck (~-14dB attenuation)
    static constexpr float kDuckGain = 0.2f;
    static constexpr float kDuckRampTolerance = 0.001f;

    size_t onAudioCallback(float* output, size_t frameCount) noexcept {
        if (state_.load(std::memory_order_acquire) != SessionState::Running) {
            return 0;
        }

        size_t written = mixer_.mix(output, frameCount, config_.localParticipantId);

        if (written > 0) {
            // Determine duck target via cached pointer (avoids Meyers-singleton guard on RT path)
            auto* smgr = sessionMgr_.load(std::memory_order_acquire);
            float targetGain = (smgr && smgr->shouldDuck()) ? kDuckGain : 1.0f;
            const size_t sampleCount = written * config::audio::kChannels;

            bool needsRamp = std::abs(targetGain - duckGain_) > kDuckRampTolerance;
            if (needsRamp) {
                // Smooth ramp from current to target over the frame (~20ms)
                float gain = duckGain_;
                float increment = (targetGain - duckGain_) / static_cast<float>(sampleCount);
                for (size_t i = 0; i < sampleCount; ++i) {
                    output[i] *= gain;
                    gain += increment;
                }
                duckGain_ = targetGain;
            } else if (targetGain < 1.0f - kDuckRampTolerance) {
                // Steady-state ducking — flat gain multiply
                for (size_t i = 0; i < sampleCount; ++i) {
                    output[i] *= targetGain;
                }
                duckGain_ = targetGain;
            }
            // targetGain ≈ 1.0 and no ramp needed → skip multiply entirely
        }

        return written;
    }

    const SessionConfig config_;
    std::mutex lifecycleMtx_;
    std::atomic<SessionState> state_{SessionState::Stopped};
    std::atomic<bool> audioPaused_{false};
    int64_t startTimeUs_ = 0;

    DecodedAudioPool audioPool_;

    std::array<std::unique_ptr<AudioDecodeSlot>, kMaxAudioSlots> audioSlotStorage_;
    std::array<AudioDecodeSlot*, kMaxAudioSlots> audioSlots_{};

    ParticipantRegistry participants_;
    SyncController sync_;

    AudioMixer mixer_;

    DecodeThread decodeThread_;
    SlotAdmission admission_;

    std::unique_ptr<AVAudioOutputBridge> audioOutput_;

    // Cached AudioSessionManager pointer for RT callback path (avoids Meyers-singleton guard)
    std::atomic<AudioSessionManager*> sessionMgr_{nullptr};

    // Current duck gain level — audio callback thread only, no synchronization needed.
    // Ramps smoothly between 1.0 (normal) and kDuckGain (0.2) to avoid clicks.
    float duckGain_ = 1.0f;

    // Audio output liveness check — JS thread only, no synchronization needed.
    int64_t lastAudioHealthCheckUs_ = 0;
};

}  // namespace media
