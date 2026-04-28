// Single-stream Opus decode channel: encoded frames in, PCM samples out.
// Three-thread design:
//   JS thread     → pushEncodedFrame() → EncodedFrameQueue (SPSC)
//   Decode thread → processPendingDecode() → DecodedAudioQueue (SPSC)
//   Audio callback → readSamples() (lock-free: no mutex, no alloc, no I/O)
// readSamples() also handles drift compensation, PLC crossfade, and fast-drain.
//
// Implementation is split across three sibling .inl files included at the
// bottom of this header:
//   AudioDecodeChannelLifecycle.inl — JS-thread state mutation: activate,
//                                     deactivate, pushEncodedFrame
//   OpusDecodePump.inl              — decode-thread orchestration:
//                                     processPendingDecode, generatePLC,
//                                     decodeFrameWithToken
//   AudioSampleReader.inl           — RT-thread output path: readSamples,
//                                     collectDecodedFrames, finalizeOutput,
//                                     readSamplesNoDrift, readSamplesWithDrift,
//                                     drainDecodedFrames, isFrameSilence
#pragma once

#include "common/MediaLog.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <cstring>
#include <cmath>
#include <algorithm>
#include "common/MediaConfig.h"
#include "MediaTypes.h"
#include "FramePool.h"
#include "OpusDecoderAdapter.h"
#include "DriftCompensator.h"
#include "DrainCrossfade.h"
#include "EncodedFrameQueue.h"
#include "FastDrainPolicy.h"
#include "PLCConcealer.h"
#include "common/CompilerHints.h"
#include "common/AVSyncCoordinator.h"
#include "common/IngestRingBuffer.h"

namespace media {

class AudioDecodeChannel {
public:
    explicit AudioDecodeChannel(DecodedAudioPool& pool) noexcept : pool_(pool) {}

    void setSyncCoordinator(AVSyncCoordinator* coordinator) noexcept {
        syncCoordinator_.store(coordinator, std::memory_order_release);
    }

    void setRingBuffer(IngestRingBuffer* ring) noexcept {
        ring_.store(ring, std::memory_order_release);
    }

    [[nodiscard]] long long lastDecodedAbsOffset() const noexcept {
        return lastDecodedAbsOffset_.load(std::memory_order_relaxed);
    }

    // The last decoded absOffset serves as the retention floor for the ring
    // buffer. Frames still queued are at higher offsets and protected by the
    // monotonic advance.  Returns 0 before the first decode.
    [[nodiscard]] long long retentionFloorAbsOffset() const noexcept {
        return lastDecodedAbsOffset_.load(std::memory_order_relaxed);
    }

    bool setDecoder(std::unique_ptr<OpusDecoderAdapter> decoder) noexcept {
        // Initialize before acquiring the lock to avoid blocking the decode thread
        // (opus_decoder_create allocates)
        if (decoder) {
            if (!decoder->initialize(config::audio::kSampleRate, config::audio::kChannels)) {
                MEDIA_LOG_E("AudioDecodeChannel: decoder init failed (err=%d)",
                            decoder->lastError());
                return false;
            }
        }
        std::lock_guard<std::mutex> lk(decoderMtx_);
        decoder_ = std::move(decoder);
        return true;
    }

    // Defined in AudioDecodeChannelLifecycle.inl
    void activate() noexcept;
    void deactivate() noexcept;
    [[nodiscard]] bool pushEncodedFrame(const RawAudioFrame& frame) noexcept;

    // Cancel a pending encoded clear (e.g., after seekTo re-feed pushes fresh frames).
    void cancelPendingEncodedClear() noexcept { encQueue_.cancelClear(); }

    // Reset ring offset tracking for a new epoch (seekTo, stream reset).
    void resetOffsetTracking() noexcept {
        lastDecodedAbsOffset_.store(0, std::memory_order_relaxed);
    }

    // Pause: keep queues, hold HW warm, output silence. No teardown.
    void pause() noexcept {
        StreamState expected = StreamState::Playing;
        if (!state_.compare_exchange_strong(expected, StreamState::Paused,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            // Also allow pausing from Underrun/Buffering
            if (expected == StreamState::Underrun || expected == StreamState::Buffering) {
                state_.store(StreamState::Paused, std::memory_order_release);
            }
        }
    }

    // Resume: transition Paused→Playing or Paused→Buffering if buffer is low.
    void resume() noexcept {
        StreamState expected = StreamState::Paused;
        if (!state_.compare_exchange_strong(expected, StreamState::Playing,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return;
        }
        // If buffer is below threshold, go to Buffering instead
        int64_t decoded = decodedDurationUs();
        int64_t target = bufferTargetUs_.load(std::memory_order_relaxed);
        if (decoded < target) {
            state_.store(StreamState::Buffering, std::memory_order_release);
        }
    }

    [[nodiscard]] bool isActive() const noexcept {
        return state_.load(std::memory_order_acquire) != StreamState::Inactive;
    }

    [[nodiscard]] StreamState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    // Service deferred encoded queue clear requested by deactivate() or activate().
    // THREADING: Must be called from DecodeThread only (SPSC consumer side).
    void serviceDeferredClear() noexcept { encQueue_.serviceDeferredClear(); }

    // Burst absorption: no trimming. Frames that exceed kMaxBufferedUs are
    // rejected at pushEncodedFrame() (hard ceiling). Within that ceiling,
    // all frames are decoded and played — the audio callback drains the
    // backlog at normal speed, letting latency rise temporarily then settle.
    // This matches Chromium WebRTC behavior and avoids audible gaps.

    // Defined in OpusDecodePump.inl
    [[nodiscard]] bool processPendingDecode() noexcept;
    [[nodiscard]] bool generatePLC() noexcept;

    // LOCK-FREE: Called from audio callback thread only. Defined in
    // AudioSampleReader.inl. Thread safety relies on:
    //   1. Audio callback checks state_ first (acquire)
    //   2. deactivate() sets state_ = Inactive to signal early exit
    //   3. Only audio callback reads/writes partial frame data
    //
    // Playback speed adjustment combines two ratio sources:
    //   1. Drift ratio: long-term clock drift compensation (±0.5%)
    //   2. Catchup ratio: burst backlog drain (up to +5%)
    //   Combined ratio > 1.0 = speed up, < 1.0 = slow down
    [[nodiscard]] size_t readSamples(float* output, size_t maxSamples) noexcept;

    [[nodiscard]] size_t decodedFrames() const noexcept { return decodedQueue_.count(); }
    [[nodiscard]] size_t encodedFrames() const noexcept { return encQueue_.count(); }
    [[nodiscard]] size_t bufferedFrames() const noexcept { return decodedFrames() + encodedFrames(); }

    [[nodiscard]] int64_t bufferedDurationUs() const noexcept {
        return static_cast<int64_t>(bufferedFrames()) * config::audio::kFrameDurationUs;
    }

    [[nodiscard]] int64_t decodedDurationUs() const noexcept {
        return static_cast<int64_t>(decodedFrames()) * config::audio::kFrameDurationUs;
    }

    [[nodiscard]] bool hasPendingDecode() const noexcept { return encQueue_.hasPendingWork(); }

    [[nodiscard]] bool needsPLC() const noexcept {
        if (!isActive()) return false;
        StreamState s = state_.load(std::memory_order_acquire);
        if (s == StreamState::Paused) return false;
        if (s != StreamState::Playing && s != StreamState::Underrun) return false;
        bool speculative = arrivalConfidence_.load(std::memory_order_relaxed)
                           >= config::speculative::kConfidenceThreshold;
        int64_t threshold = speculative
            ? config::audio::kFrameDurationUs
            : config::audio::kPLCThresholdUs;
        return decodedDurationUs() < threshold;
    }

    void setGain(float gain) noexcept {
        targetGain_.store(std::clamp(gain, config::mix::kMinGain, config::mix::kMaxGain),
                          std::memory_order_relaxed);
    }

    [[nodiscard]] float targetGain() const noexcept {
        return targetGain_.load(std::memory_order_relaxed);
    }

    void setArrivalConfidence(float c) noexcept {
        arrivalConfidence_.store(c, std::memory_order_relaxed);
    }

    void setBufferTarget(int64_t targetUs) noexcept {
        bufferTargetUs_.store(
            std::clamp(targetUs, config::jitter::kMinBufferUs, config::jitter::kMaxBufferUs),
            std::memory_order_relaxed);
    }

    void setCatchupPolicy(CatchupPolicy p) noexcept { drainPolicy_.setPolicy(p); }

    void setPlaybackRate(float rate) noexcept {
        playbackRate_.store(
            std::clamp(rate, config::playbackrate::kMinRate, config::playbackrate::kMaxRate),
            std::memory_order_relaxed);
    }

    [[nodiscard]] float playbackRate() const noexcept {
        return playbackRate_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] int64_t bufferTarget() const noexcept {
        return bufferTargetUs_.load(std::memory_order_relaxed);
    }

    // Clock drift compensation for long-duration calls
    void updateDriftCompensation(int64_t cumulativeDriftUs, int64_t measurementWindowUs,
                                  uint32_t sampleCount) noexcept {
        driftCompensator_.updateDrift(cumulativeDriftUs, measurementWindowUs, sampleCount);
    }

    [[nodiscard]] int32_t driftPpm() const noexcept {
        return driftCompensator_.driftPpm();
    }

    [[nodiscard]] bool isDriftCompensationActive() const noexcept {
        return driftCompensator_.isActive();
    }

    // Request drift state reset from the audio callback thread.
    // Called when audio route changes to prevent stale drift ratio from
    // causing ~1-2s of wrong playback speed after the route transition.
    // Safe to call from any thread (sets atomic flag consumed by readSamples).
    void requestDriftReset() noexcept {
        driftResetRequested_.store(true, std::memory_order_release);
    }

    // Request encoded queue clear from the decode thread (SPSC-safe).
    // Used by resetStreamState() to flush stale frames from a previous stream.
    void requestEncodedClear() noexcept { encQueue_.requestClear(); }

    // Snapshot of current drift ratio for metrics (any thread, read-only).
    [[nodiscard]] float driftCurrentRatio() const noexcept {
        return driftCompensator_.smoothedRatio();
    }

    // Snapshot of current catchup ratio for metrics (any thread, read-only).
    [[nodiscard]] float catchupCurrentRatio() const noexcept {
        return catchup_.currentRatio();
    }

    [[nodiscard]] uint64_t catchupDeadZoneSnaps() const noexcept {
        return catchup_.deadZoneSnaps();
    }

    // Per-callback silence event counter — bumped from the audio mixer every
    // time the callback produced zero samples (regardless of state). Distinct
    // from `underruns`, which counts only Playing→Underrun edge transitions.
    // The triage trees in production-triage/SKILL.md treat "audio glitch /
    // sustained silence" as an event-rate diagnosis, which the edge counter
    // structurally undercounts.
    void markSilenceCallback() noexcept {
        metrics_.silenceCallbacks.fetch_add(1, std::memory_order_relaxed);
    }

    void markUnderrun() noexcept {
        bool speculative = arrivalConfidence_.load(std::memory_order_relaxed)
                           >= config::speculative::kConfidenceThreshold;
        if (speculative) {
            uint32_t holds = ++speculativeHoldCount_;
            if (holds <= config::speculative::kSpeculativePLCHoldFrames) {
                return;
            }
        }
        speculativeHoldCount_ = 0;
        StreamState prev = state_.exchange(StreamState::Underrun, std::memory_order_acq_rel);
        if (prev != StreamState::Underrun) {
            metrics_.underruns.fetch_add(1, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] int64_t lastRenderedPtsUs() const noexcept {
        return lastRenderedPtsUs_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] const StreamMetrics& metrics() const noexcept { return metrics_; }

    [[nodiscard]] uint32_t currentConsecutivePLC() const noexcept {
        return consecutivePLCFrames_;
    }

    [[nodiscard]] bool isDecodedPoolUnderPressure() const noexcept {
        return pool_.isUnderPressure();
    }

    [[nodiscard]] bool isEncodedPoolUnderPressure() const noexcept {
        return encQueue_.isPoolUnderPressure();
    }

    // Block until the decode thread completes any in-progress decode cycle.
    // After this returns with ring_ == nullptr, no decode thread access to
    // the ring buffer is possible. Call from JS thread only.
    void waitForDecodeIdle() noexcept {
        std::lock_guard<std::mutex> lk(decoderMtx_);
    }

#ifdef UNIT_TEST
    std::unique_lock<std::mutex> testLockDecoder() noexcept {
        return std::unique_lock<std::mutex>(decoderMtx_);
    }
#endif

private:
    // Result of collecting decoded frames into a buffer.
    struct CollectResult {
        size_t samplesCollected;
        int64_t lastRenderPts;
        size_t silenceSkips;
    };

    // Defined in AudioSampleReader.inl
    [[nodiscard]] HOT_FUNCTION
    CollectResult collectDecodedFrames(float* dest, size_t maxSamples) noexcept;
    void finalizeOutput(float* output, size_t written, int64_t lastRenderPts, size_t silenceSkips) noexcept;
    [[nodiscard]] size_t readSamplesNoDrift(float* output, size_t maxSamples) noexcept;
    [[nodiscard]] size_t readSamplesWithDrift(float* output, size_t maxSamples, float ratio) noexcept;
    size_t drainDecodedFrames(size_t maxFrames) noexcept;
    static bool isFrameSilence(const DecodedAudioFrame* frame) noexcept;

    // Defined in OpusDecodePump.inl
    [[nodiscard]] bool decodeFrameWithToken(long long absOffset, size_t size,
                                              int64_t ptsUs, int64_t durationUs,
                                              DecodedAudioToken token) noexcept;

    DecodedAudioPool& pool_;

    EncodedFrameQueue encQueue_;

    DecodedAudioQueue decodedQueue_;
    std::unique_ptr<OpusDecoderAdapter> decoder_;

    std::mutex decoderMtx_;

    alignas(config::kCacheLineSize) std::atomic<StreamState> state_{StreamState::Inactive};

    alignas(config::kCacheLineSize) std::atomic<int64_t> lastPtsUs_{config::sentinel::kNoTimestamp};
    std::atomic<int64_t> lastDecodedPtsUs_{0};
    std::atomic<int64_t> lastPushTimeUs_{0};  // wall-clock time of last pushEncodedFrame call
    alignas(config::kCacheLineSize) std::atomic<int64_t> lastRenderedPtsUs_{0};
    std::atomic<bool> needsDecoderReset_{false};
    // Deferred decoded queue clear requested by activate(). Serviced by audio callback
    // via readSamples() to avoid SPSC contract violation — activate() runs on the JS
    // thread while the audio callback is the sole consumer of decodedQueue_.
    std::atomic<bool> decodedClearRequested_{false};
    // Deferred drift state reset requested by route change. Serviced by audio
    // callback in readSamples() to reset stale drift ratio without clearing
    // buffered audio. Set from JS thread (route callback), read from audio thread.
    std::atomic<bool> driftResetRequested_{false};

    alignas(config::kCacheLineSize) std::atomic<float> targetGain_{config::mix::kDefaultGain};
    std::atomic<int64_t> bufferTargetUs_{config::jitter::kDefaultBufferUs};
    std::atomic<float> arrivalConfidence_{0.0f};
    FastDrainPolicy drainPolicy_;
    std::atomic<float> playbackRate_{config::playbackrate::kDefaultRate};

    StreamMetrics metrics_;

    // Speculative playback hold counter — audio callback thread only
    uint32_t speculativeHoldCount_{0};

    // Partial frame state - accessed only by audio callback thread
    DecodedAudioToken partialToken_;
    size_t partialOffset_{0};
    std::atomic<bool> partialClearRequested_{false};

    // Clock drift compensation - DriftCompensator is updated from JS thread,
    // SpeexDriftResampler + CatchupController accessed only from audio callback thread
    DriftCompensator driftCompensator_;
    CatchupController catchup_;
    SpeexDriftResampler resampler_;

    // Pre-allocated buffer for drift/catchup resampling (audio callback only)
    alignas(config::kSimdAlignment) std::array<float, config::audio::kFrameSize * 8> driftBuffer_{};
    // Atomic: written by audio callback (readSamplesWithDrift), zeroed by activate() from JS thread
    std::atomic<size_t> driftBufferLeftover_{0};

    // Fast-drain crossfade — audio callback thread only, no synchronization.
    DrainCrossfade drain_;

    // PLC→real crossfade — audio callback thread only, no synchronization.
    PLCConcealer plc_;

    // Fast-path / resampled-path hysteresis — prevents oscillation when
    // combined ratio hovers near unity (common on Bluetooth SCO).
    uint8_t fastPathHoldCount_{0};
    bool lastWasFastPath_{true};

    // Consecutive PLC frame counter — decode thread only (under decoderMtx_)
    uint32_t consecutivePLCFrames_{0};

    // Scratch buffer for linearizing encoded Opus packets when the ring-buffer read
    // wraps around. Decode thread only; kept as a member to avoid a 1.2KB stack burn
    // per decode call.
    alignas(config::kSimdAlignment) std::array<uint8_t, config::audio::kMaxEncodedFrameSize> decodeLinearBuf_{};

    // A/V sync coordinator — nullable, set when video pipeline is active.
    // Audio callback reports render PTS via atomic stores (safe from RT thread).
    std::atomic<AVSyncCoordinator*> syncCoordinator_{nullptr};

    // Ring buffer for zero-copy encoded packet access (set by MediaSession).
    std::atomic<IngestRingBuffer*> ring_{nullptr};

    // Tracks the absOffset of the last successfully decoded frame.
    // Used as the retention floor for ring buffer compaction — all queued
    // frames are at higher offsets and thus naturally protected.
    std::atomic<long long> lastDecodedAbsOffset_{0};
};

}  // namespace media

// Out-of-class member-function definitions. Included after the class body so
// each .inl has full visibility of state and types.
#include "AudioDecodeChannelLifecycle.inl"
#include "OpusDecodePump.inl"
#include "AudioSampleReader.inl"
