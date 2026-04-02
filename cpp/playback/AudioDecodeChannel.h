// Single-stream Opus decode channel: encoded frames in, PCM samples out.
// Three-thread design:
//   JS thread     → pushEncodedFrame() → PendingAudioQueue (SPSC)
//   Decode thread → processPendingDecode() → DecodedAudioQueue (SPSC)
//   Audio callback → readSamples() (lock-free: no mutex, no alloc, no I/O)
// readSamples() also handles drift compensation, PLC crossfade, and fast-drain.
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
#include "common/CompilerHints.h"
#include "common/AVSyncCoordinator.h"

namespace media {

class AudioDecodeChannel {
public:
    explicit AudioDecodeChannel(DecodedAudioPool& pool) noexcept : pool_(pool) {}

    void setSyncCoordinator(AVSyncCoordinator* coordinator) noexcept {
        syncCoordinator_.store(coordinator, std::memory_order_release);
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

    void activate() noexcept {
        std::lock_guard<std::mutex> lk(decoderMtx_);

        // Transition to Inactive first so concurrent readSamples() exits early
        state_.store(StreamState::Inactive, std::memory_order_release);

        lastPtsUs_.store(config::sentinel::kNoTimestamp, std::memory_order_relaxed);
        lastDecodedPtsUs_.store(0, std::memory_order_relaxed);
        lastPushTimeUs_.store(0, std::memory_order_relaxed);
        lastRenderedPtsUs_.store(0, std::memory_order_relaxed);
        targetGain_.store(config::mix::kDefaultGain, std::memory_order_relaxed);
        bufferTargetUs_.store(config::jitter::kDefaultBufferUs, std::memory_order_relaxed);

        // Do NOT clear encodedQueue_ here — serviceDeferredClear() on the decode
        // thread may be mid-clear(). Only the decode thread touches the consumer
        // side of encodedQueue_. Set the flag so the decode thread clears it on
        // its next cycle.
        encodedClearRequested_.store(true, std::memory_order_release);

        // Do NOT clear decodedQueue_ here — the audio callback is the sole SPSC
        // consumer and may be mid-pop() in readSamples(). Defer the clear to the
        // audio callback via decodedClearRequested_. The audio callback services
        // this flag before the state check in readSamples(), ensuring stale frames
        // are drained before the decode thread pushes new frames.
        decodedClearRequested_.store(true, std::memory_order_release);

        // Do NOT call clearPartialFrameUnsafe() here — partial frame state
        // (partialToken_, partialOffset_) is owned by the audio callback thread.
        // A concurrent readSamples() call may be actively reading the partial
        // frame. partialClearRequested_ signals the audio callback to release
        // partial state on its next run.
        partialClearRequested_.store(true, std::memory_order_release);

        // Reset drift compensator (atomic-only state, safe from any thread)
        driftCompensator_.reset();

        // Ensure resampler is allocated for first-time use. init() calls
        // speex_resampler_init() which allocates — must happen here, not on
        // the audio callback thread. Only runs when state_ == nullptr (first
        // activate or after destroy), so no concurrent audio callback access.
        if (!resampler_.isInitialized()) resampler_.init();

        // Do NOT call catchup_.reset() or resampler_.reset() here — these
        // modify audio-callback-only state and would race with readSamples().
        // The decodedClearRequested_ handler in readSamples() resets them from
        // the correct thread.
        driftBufferLeftover_.store(0, std::memory_order_relaxed);

        // Clear any pending reset flag from previous session
        needsDecoderReset_.store(false, std::memory_order_relaxed);

        if (decoder_) decoder_->reset();
        consecutivePLCFrames_ = 0;

        state_.store(StreamState::Buffering, std::memory_order_release);

        MEDIA_LOG_D("AudioDecodeChannel: activated");
    }

    // Non-blocking deactivate. Thread safety guaranteed by release/acquire ordering:
    // 1. state_ set to Inactive causes readSamples() and pushEncodedFrame() to exit early
    // 2. partialClearRequested_ signals audio callback to release partial frame
    // 3. encodedClearRequested_ signals decode thread to clear encoded queue
    void deactivate() noexcept {
        std::lock_guard<std::mutex> lk(decoderMtx_);

        if (state_.load(std::memory_order_relaxed) == StreamState::Inactive) return;

        // Signal audio callback to stop reading - release ensures visibility
        state_.store(StreamState::Inactive, std::memory_order_release);

        // Request partial frame cleanup - audio callback will process on next run
        partialClearRequested_.store(true, std::memory_order_release);

        // Do NOT clear encodedQueue_ here — the decode thread may be mid-pop()
        // in processPendingDecode() (which runs outside decoderMtx_). Instead, defer
        // the clear to the decode thread via encodedClearRequested_. The decode
        // thread checks this flag before touching encodedQueue_ each cycle.
        // Do NOT clear decodedQueue_ here — the audio callback may still be mid-pop().
        // activate() defers the clear to the audio callback via decodedClearRequested_.
        encodedClearRequested_.store(true, std::memory_order_release);

        if (decoder_) decoder_->reset();

        MEDIA_LOG_D("AudioDecodeChannel: deactivated");
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

    [[nodiscard]] bool pushEncodedFrame(const RawAudioFrame& frame) noexcept {
        if (!isActive()) return false;

        if (frame.size == 0 || frame.size > config::audio::kMaxEncodedFrameSize) {
            metrics_.framesDropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        // Track inter-frame delivery gap for diagnosing upstream stalls.
        // Uses wall-clock time (not PTS) to measure actual JS→native delivery timing.
        {
            int64_t now = nowUs();
            int64_t prev = lastPushTimeUs_.load(std::memory_order_relaxed);
            lastPushTimeUs_.store(now, std::memory_order_relaxed);
            if (prev > 0) {
                int64_t gapUs = now - prev;
                // Update max gap (relaxed CAS loop — rare contention, JS thread only)
                int64_t curMax = metrics_.maxInterFrameGapUs.load(std::memory_order_relaxed);
                while (gapUs > curMax &&
                       !metrics_.maxInterFrameGapUs.compare_exchange_weak(
                           curMax, gapUs, std::memory_order_relaxed)) {}
                if (gapUs > 500000) {
                    metrics_.gapsOver500ms.fetch_add(1, std::memory_order_relaxed);
                    metrics_.gapsOver100ms.fetch_add(1, std::memory_order_relaxed);
                    metrics_.gapsOver50ms.fetch_add(1, std::memory_order_relaxed);
                } else if (gapUs > 100000) {
                    metrics_.gapsOver100ms.fetch_add(1, std::memory_order_relaxed);
                    metrics_.gapsOver50ms.fetch_add(1, std::memory_order_relaxed);
                } else if (gapUs > 50000) {
                    metrics_.gapsOver50ms.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        // DTX frames are valid Opus packets that decode to comfort noise.
        // Pass them through to the decoder - do not skip them.

        int64_t buffered = bufferedDurationUs();
        if (buffered >= config::audio::kMaxBufferedUs) {
            uint64_t drops = metrics_.framesDropped.fetch_add(1, std::memory_order_relaxed) + 1;
            // Log first drop immediately (drops==1), then every kDropLogInterval drops.
            // Pattern: 1, 65, 129, 193... for interval=64
            if ((drops & (config::logging::kDropLogInterval - 1)) == 1) {
                MEDIA_LOG_W("AudioDecodeChannel: buffer full (%lldus), dropped %llu",
                            static_cast<long long>(buffered),
                            static_cast<unsigned long long>(drops));
            }
            return false;
        }

        int64_t lastPts = lastPtsUs_.load(std::memory_order_relaxed);
        if (lastPts != config::sentinel::kNoTimestamp) {
            int64_t delta = frame.timestampUs - lastPts;
            if (delta < -config::epoch::kMaxBackwardJumpUs || delta > config::epoch::kMaxForwardJumpUs) {
                MEDIA_LOG_I("AudioDecodeChannel: PTS discontinuity (delta=%lldms), resetting",
                            static_cast<long long>(delta / 1000));
                needsDecoderReset_.store(true, std::memory_order_release);
                metrics_.ptsDiscontinuities.fetch_add(1, std::memory_order_relaxed);
            }
        }
        lastPtsUs_.store(frame.timestampUs, std::memory_order_relaxed);

        auto token = encodedPool_.acquire();
        if (!token) {
            metrics_.framesDropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        PendingAudioFrame* pending = token.get();
        size_t copySize = std::min(frame.size, pending->data.size());
        std::memcpy(pending->data.data(), frame.data, copySize);
        pending->size = copySize;
        pending->ptsUs = frame.timestampUs;
        pending->durationUs = frame.durationUs;

        if (!encodedQueue_.push(std::move(token))) {
            metrics_.framesDropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        metrics_.framesReceived.fetch_add(1, std::memory_order_relaxed);
        decodeWorkPending_.store(true, std::memory_order_release);
        return true;
    }

    // Service deferred encoded queue clear requested by deactivate() or activate().
    // THREADING: Must be called from DecodeThread only (SPSC consumer of encodedQueue_).
    // Neither deactivate() nor activate() can call clear() directly because
    // processPendingDecode() pops from encodedQueue_ on the decode thread —
    // concurrent clear() from the JS thread would violate the SPSC
    // single-consumer contract.
    void serviceDeferredClear() noexcept {
        if (LIKELY(!encodedClearRequested_.load(std::memory_order_acquire))) return;
        encodedQueue_.clear();
        encodedClearRequested_.store(false, std::memory_order_release);
    }

    // Burst absorption: no trimming. Frames that exceed kMaxBufferedUs are
    // rejected at pushEncodedFrame() (hard ceiling). Within that ceiling,
    // all frames are decoded and played — the audio callback drains the
    // backlog at normal speed, letting latency rise temporarily then settle.
    // This matches Chromium WebRTC behavior and avoids audible gaps.

    [[nodiscard]] bool processPendingDecode() noexcept {
        if (!decodeWorkPending_.load(std::memory_order_acquire)) return false;
        if (!isActive()) return false;

        // try_lock avoids priority inversion: decode thread (SCHED_FIFO) must
        // never block on the JS thread holding decoderMtx_ during lifecycle ops.
        // If contended, skip this cycle — decode thread retries in ~500us.
        std::unique_lock<std::mutex> lk(decoderMtx_, std::try_to_lock);
        if (!lk.owns_lock()) return false;

        if (needsDecoderReset_.exchange(false, std::memory_order_acq_rel)) {
            if (decoder_) decoder_->reset();
            consecutivePLCFrames_ = 0;

            // Do NOT clear decodedQueue_ — decoded PCM is valid audio regardless
            // of PTS epoch change. Let audio callback drain existing frames naturally.
            // Only the decoder state needs resetting (stateful codec).

            // Reset lastDecodedPtsUs so gap detection doesn't fire on the first
            // frame of the new epoch (which would have a large PTS jump from
            // the last decoded frame of the old epoch).
            lastDecodedPtsUs_.store(0, std::memory_order_relaxed);
        }

        bool didWork = false;
        while (true) {
            // Decoded frame cap: prevent exhausting
            // the shared pool. Without this, continuous Buffering state
            // Cap decoded frames to avoid exhausting the pool.
            if (decodedFrames() >= config::audio::kDecodeQueueDepth) break;

            // Reserve a decoded pool token BEFORE popping the encoded queue.
            // This is the authoritative pool check — CAS-based acquire is exact,
            // unlike the approximate available() counter. If acquire fails, encoded
            // frames stay queued for the next decode cycle (~500us later) when the
            // audio callback frees pool entries.
            auto decodedToken = pool_.acquire();
            if (!decodedToken) break;

            auto token = encodedQueue_.pop();
            if (!token) {
                // No encoded frames — release the pre-acquired token back to pool.
                // Token destructor handles this automatically via RAII.
                break;
            }

            PendingAudioFrame* pending = token.get();

            if (decodeFrameWithToken(pending->data.data(), pending->size,
                                     pending->ptsUs, pending->durationUs,
                                     std::move(decodedToken))) {
                didWork = true;
            }
        }

        // Only clear decodeWorkPending_ if all work is truly done.
        // Two conditions break the decode loop with work remaining:
        //   1. Pool exhaustion — no tokens available
        //   2. Cap — enough decoded frames
        // In both cases, keep the flag set so DecodeThread re-enters on the
        // next cycle (~500us) when the audio callback frees pool entries.
        bool workRemains = encodedQueue_.count() > 0;
        if (!workRemains) {
            decodeWorkPending_.store(false, std::memory_order_release);
        }
        return didWork;
    }

    [[nodiscard]] bool generatePLC() noexcept {
        // try_lock avoids priority inversion (see processPendingDecode comment).
        std::unique_lock<std::mutex> lk(decoderMtx_, std::try_to_lock);
        if (!lk.owns_lock()) return false;

        if (!decoder_ || !isActive()) return false;

        // After kMaxConsecutivePLC frames, Opus PLC quality degrades badly.
        // Reset decoder and return false to let the mixer's CNG handle it.
        if (consecutivePLCFrames_ >= config::plc::kMaxConsecutivePLC) {
            decoder_->reset();
            return false;
        }

        auto token = pool_.acquire();
        if (!token) return false;

        DecodedAudioFrame* frame = token.get();
        int decoded = decoder_->decodePLC(frame->samples.data(), config::audio::kFrameSamples);
        if (decoded <= 0) return false;

        ++consecutivePLCFrames_;
        metrics_.plcFrames.fetch_add(1, std::memory_order_relaxed);
        uint32_t prev = metrics_.peakConsecutivePLC.load(std::memory_order_relaxed);
        if (consecutivePLCFrames_ > prev) {
            metrics_.peakConsecutivePLC.store(consecutivePLCFrames_, std::memory_order_relaxed);
        }

        frame->sampleCount = static_cast<uint32_t>(decoded) * static_cast<uint32_t>(config::audio::kChannels);
        frame->ptsUs = lastDecodedPtsUs_.load(std::memory_order_relaxed) +
                       config::audio::kFrameDurationUs;
        frame->durationUs = config::audio::kFrameDurationUs;
        frame->isConcealed = true;

        lastDecodedPtsUs_.store(frame->ptsUs, std::memory_order_relaxed);

        return decodedQueue_.push(std::move(token));
    }

    // LOCK-FREE: Called from audio callback thread only
    // Thread safety relies on:
    //   1. Audio callback checks state_ first (acquire)
    //   2. deactivate() sets state_ = Inactive to signal early exit
    //   3. Only audio callback reads/writes partial frame data
    //
    // Playback speed adjustment combines two ratio sources:
    //   1. Drift ratio: long-term clock drift compensation (±0.5%)
    //   2. Catchup ratio: burst backlog drain (up to +5%)
    //   Combined ratio > 1.0 = speed up, < 1.0 = slow down
    [[nodiscard]] size_t readSamples(float* output, size_t maxSamples) noexcept {
        if (!isActive()) return 0;

        // Service deferred decoded queue clear from activate() BEFORE the state check.
        // activate() sets state_ = Buffering, which would cause the early return below
        // to skip this clear — leaving stale frames from a previous session in the queue
        // where the decode thread would push new frames on top of them.
        // Uses pop() loop to maintain SPSC contract: this thread
        // is the sole consumer of decodedQueue_.
        if (UNLIKELY(decodedClearRequested_.load(std::memory_order_acquire))) {
            decodedQueue_.clear();
            partialToken_.release();
            partialOffset_ = 0;
            driftBufferLeftover_.store(0, std::memory_order_relaxed);
            resampler_.reset();
            catchup_.reset();
            drainFadePending_ = false;
            drainActive_ = false;
            plcFadePending_ = false;
            lastFrameWasConcealed_ = false;
            fastPathHoldCount_ = 0;
            lastWasFastPath_ = true;
            // Clear partialClearRequested_ too — activate() sets both flags, and
            // we've already cleaned up all partial state above.
            partialClearRequested_.store(false, std::memory_order_relaxed);
            decodedClearRequested_.store(false, std::memory_order_release);
        }

        // Reset drift state after route change without clearing buffered audio.
        // Preserves decoded queue contents (no audible gap) while preventing
        // stale drift ratio from causing wrong playback speed post-transition.
        if (UNLIKELY(driftResetRequested_.load(std::memory_order_acquire))) {
            driftCompensator_.reset();
            driftBufferLeftover_.store(0, std::memory_order_relaxed);
            resampler_.reset();
            catchup_.resetRatio();
            driftResetRequested_.store(false, std::memory_order_release);
        }

        // Don't drain buffer while still accumulating initial jitter depth
        StreamState s = state_.load(std::memory_order_acquire);
        if (UNLIKELY(s == StreamState::Buffering || s == StreamState::Inactive || s == StreamState::Paused)) return 0;

        if (UNLIKELY(partialClearRequested_.load(std::memory_order_acquire))) {
            partialToken_.release();
            partialOffset_ = 0;
            driftBufferLeftover_.store(0, std::memory_order_relaxed);
            resampler_.reset();
            catchup_.reset();
            plcFadePending_ = false;
            lastFrameWasConcealed_ = false;
            partialClearRequested_.store(false, std::memory_order_release);
        }

        // Fast-drain: when buffer depth far exceeds target, discard decoded
        // frames to snap latency back toward target. Each discarded frame frees
        // a pool token so the decode thread refills on its next cycle (~500us).
        // A single 5ms crossfade splice at the burst start prevents audible clicks.
        {
            int64_t totalBuffered = bufferedDurationUs();
            int64_t target = bufferTargetUs_.load(std::memory_order_relaxed);
            int64_t policyThreshold;
            switch (catchupPolicy_.load(std::memory_order_relaxed)) {
                case CatchupPolicy::Accelerate:  policyThreshold = config::fastdrain::kExcessThresholdAccelerate; break;
                case CatchupPolicy::DropToLive:   policyThreshold = config::fastdrain::kExcessThresholdDropToLive; break;
                default:                          policyThreshold = config::fastdrain::kExcessThresholdPlayThrough; break;
            }
            int64_t excess = totalBuffered - target - policyThreshold;
            if (UNLIKELY(excess > 0)) {
                size_t framesToDrain = static_cast<size_t>(excess / config::audio::kFrameDurationUs);
                // Reserve at least 1 decoded frame for output to prevent underrun
                size_t available = decodedFrames() + (partialToken_ ? 1 : 0);
                size_t maxDrain = available > 1 ? available - 1 : 0;
                framesToDrain = std::min(framesToDrain, maxDrain);
                if (framesToDrain > 0) {
                    drainDecodedFrames(framesToDrain);
                    // Discard stale pre-drain samples in the Speex drift buffer.
                    // Without this, old audio from before the skip would be output
                    // before the new post-drain audio, creating a temporal seam.
                    driftBufferLeftover_.store(0, std::memory_order_relaxed);
                    // Flush Speex resampler filter state to prevent stale pre-drain
                    // audio leaking into the first post-drain output (filter memory
                    // contains samples from before the time skip).
                    resampler_.reset();
                    // Reset catchup ratio to unity so the post-drain callbacks
                    // don't inherit a stale elevated ratio. Without this, the
                    // smoothed ratio (α=0.02) takes ~50 callbacks (~1s) to decay,
                    // unnecessarily speeding up playback on an already-correct
                    // buffer depth.
                    // resetRatio() (not reset()) preserves the cumulative
                    // dead-zone snap counter for session-lifetime diagnostics.
                    catchup_.resetRatio();
                }
            } else {
                // Burst drain complete — reset for next burst
                drainActive_ = false;
            }
        }

        // Compute combined ratio: drift * catchup * user playback rate
        float driftRatio = driftCompensator_.currentRatio();
        float catchupRatio = catchup_.update(
            bufferedDurationUs(), bufferTargetUs_.load(std::memory_order_relaxed));
        float userRate = playbackRate_.load(std::memory_order_relaxed);
        float combinedRatio = driftRatio * catchupRatio * userRate;

        // Fast path: no resampling needed (ratio ~= 1.0).
        // Hysteresis prevents oscillation between fast/resampled paths when
        // the combined ratio hovers near unity (common on Bluetooth where
        // CatchupController smoothing produces micro-fluctuations).
        bool wantFastPath = std::abs(combinedRatio - 1.0f) < config::catchup::kFastPathUnityThreshold;
        if (wantFastPath != lastWasFastPath_) {
            if (++fastPathHoldCount_ < 3) {
                wantFastPath = lastWasFastPath_;  // hold previous mode
            } else {
                fastPathHoldCount_ = 0;
                lastWasFastPath_ = wantFastPath;
                metrics_.fastPathSwitches.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            fastPathHoldCount_ = 0;
        }

        if (LIKELY(wantFastPath)) {
            // Drain any leftover samples from the Speex resampler path before
            // switching to direct copy. Without this, samples buffered inside
            // driftBuffer_ from a previous resampled callback would be lost,
            // causing a small audible discontinuity at the transition.
            size_t leftover = driftBufferLeftover_.load(std::memory_order_relaxed);
            if (UNLIKELY(leftover > 0)) {
                size_t toCopy = std::min(leftover, maxSamples);
                std::memcpy(output, driftBuffer_.data(), toCopy * sizeof(float));
                if (toCopy < leftover) {
                    std::memmove(driftBuffer_.data(), driftBuffer_.data() + toCopy,
                                 (leftover - toCopy) * sizeof(float));
                    driftBufferLeftover_.store(leftover - toCopy, std::memory_order_relaxed);
                } else {
                    driftBufferLeftover_.store(0, std::memory_order_relaxed);
                }
                metrics_.samplesOutput.fetch_add(toCopy, std::memory_order_relaxed);
                if (toCopy >= maxSamples) return toCopy;
                // readSamplesNoDrift handles its own metrics accounting
                return toCopy + readSamplesNoDrift(output + toCopy, maxSamples - toCopy);
            }
            return readSamplesNoDrift(output, maxSamples);
        }

        // Resampled path: use Speex resampler for drift + catchup
        return readSamplesWithDrift(output, maxSamples, combinedRatio);
    }

private:
    // Fast path: direct copy without drift compensation
    [[nodiscard]] HOT_FUNCTION
    size_t readSamplesNoDrift(float* output, size_t maxSamples) noexcept {
        size_t written = 0;

        // Consume any remaining samples from partial frame
        if (partialToken_ && partialOffset_ < partialToken_->sampleCount) {
            DecodedAudioFrame* frame = partialToken_.get();
            size_t remaining = frame->sampleCount - partialOffset_;
            size_t toCopy = std::min(maxSamples, remaining);

            std::memcpy(output, frame->samples.data() + partialOffset_, toCopy * sizeof(float));
            written += toCopy;
            partialOffset_ += toCopy;

            if (partialOffset_ >= frame->sampleCount) {
                partialToken_.release();
                partialOffset_ = 0;
            }
        }

        int64_t lastRenderPts = 0;

        while (LIKELY(written < maxSamples)) {
            auto token = decodedQueue_.pop();
            if (!token) break;

            DecodedAudioFrame* frame = token.get();

            // Detect PLC→real transition: save tail of concealed output for crossfade
            if (UNLIKELY(lastFrameWasConcealed_ && !frame->isConcealed)) {
                savePlcFadeTail(output, written);
            }
            lastFrameWasConcealed_ = frame->isConcealed;

            if (!frame->isConcealed && frame->ptsUs > 0) {
                lastRenderPts = frame->ptsUs;
            }

            size_t toCopy = std::min(maxSamples - written, static_cast<size_t>(frame->sampleCount));
            std::memcpy(output + written, frame->samples.data(), toCopy * sizeof(float));
            written += toCopy;

            if (UNLIKELY(toCopy < frame->sampleCount)) {
                partialToken_ = std::move(token);
                partialOffset_ = toCopy;
                break;
            }
        }

        if (written > 0) {
            applyDrainCrossfade(output, written);
            applyPlcCrossfade(output, written);
            metrics_.samplesOutput.fetch_add(written, std::memory_order_relaxed);

            // Report audio render PTS for A/V sync (atomic stores only — RT-safe)
            if (lastRenderPts > 0) {
                lastRenderedPtsUs_.store(lastRenderPts, std::memory_order_relaxed);
                auto* sync = syncCoordinator_.load(std::memory_order_acquire);
                if (sync) {
                    sync->onAudioRender(lastRenderPts, nowUs());
                }
            }
        }

        return written;
    }

    // Resampled path: Speex resampler for drift + catchup speed adjustment
    [[nodiscard]] size_t readSamplesWithDrift(float* output, size_t maxSamples, float ratio) noexcept {
        size_t outputWritten = 0;

        if (UNLIKELY(!std::isfinite(ratio) || ratio <= 0.0f || ratio > 2.0f)) {
            ratio = 1.0f;
        }

        // Update Speex resampler ratio
        resampler_.setRatio(ratio);

        // Calculate how many input samples we need to produce maxSamples output
        // (with margin for resampler filter delay)
        size_t inputNeeded = static_cast<size_t>(static_cast<float>(maxSamples) * ratio) + 4;
        inputNeeded = std::min(inputNeeded, driftBuffer_.size());

        // Collect input samples into drift buffer (preserving leftover from previous call)
        size_t inputCollected = driftBufferLeftover_.load(std::memory_order_relaxed);
        float* driftBuf = driftBuffer_.data();

        // First: drain partial frame if any
        if (inputCollected < inputNeeded && partialToken_ && partialOffset_ < partialToken_->sampleCount) {
            DecodedAudioFrame* frame = partialToken_.get();
            size_t remaining = frame->sampleCount - partialOffset_;
            size_t toCopy = std::min(inputNeeded - inputCollected, remaining);

            std::memcpy(driftBuf + inputCollected, frame->samples.data() + partialOffset_, toCopy * sizeof(float));
            inputCollected += toCopy;
            partialOffset_ += toCopy;

            if (partialOffset_ >= frame->sampleCount) {
                partialToken_.release();
                partialOffset_ = 0;
            }
        }

        // Second: drain decoded queue
        int64_t lastRenderPts = 0;

        while (inputCollected < inputNeeded) {
            auto token = decodedQueue_.pop();
            if (!token) break;

            DecodedAudioFrame* frame = token.get();

            // Detect PLC→real transition for crossfade
            if (UNLIKELY(lastFrameWasConcealed_ && !frame->isConcealed)) {
                savePlcFadeTail(driftBuf, inputCollected);
            }
            lastFrameWasConcealed_ = frame->isConcealed;

            if (!frame->isConcealed && frame->ptsUs > 0) {
                lastRenderPts = frame->ptsUs;
            }

            size_t toCopy = std::min(inputNeeded - inputCollected,
                                     static_cast<size_t>(frame->sampleCount));

            std::memcpy(driftBuf + inputCollected, frame->samples.data(), toCopy * sizeof(float));
            inputCollected += toCopy;

            if (toCopy < frame->sampleCount) {
                partialToken_ = std::move(token);
                partialOffset_ = toCopy;
                break;
            }
        }

        // Apply Speex resampling
        if (inputCollected > 0) {
            size_t inputConsumed = 0;
            outputWritten = resampler_.process(driftBuf, inputCollected,
                                               output, maxSamples,
                                               inputConsumed);

            if (inputConsumed < inputCollected) {
                size_t leftover = inputCollected - inputConsumed;
                std::memmove(driftBuf, driftBuf + inputConsumed, leftover * sizeof(float));
                driftBufferLeftover_.store(leftover, std::memory_order_relaxed);
            } else {
                driftBufferLeftover_.store(0, std::memory_order_relaxed);
            }
        }

        if (outputWritten > 0) {
            applyDrainCrossfade(output, outputWritten);
            applyPlcCrossfade(output, outputWritten);
            metrics_.samplesOutput.fetch_add(outputWritten, std::memory_order_relaxed);

            if (lastRenderPts > 0) {
                lastRenderedPtsUs_.store(lastRenderPts, std::memory_order_relaxed);
                auto* sync = syncCoordinator_.load(std::memory_order_acquire);
                if (sync) {
                    sync->onAudioRender(lastRenderPts, nowUs());
                }
            }
        }

        return outputWritten;
    }

public:

    [[nodiscard]] size_t decodedFrames() const noexcept { return decodedQueue_.count(); }
    [[nodiscard]] size_t encodedFrames() const noexcept { return encodedQueue_.count(); }
    [[nodiscard]] size_t bufferedFrames() const noexcept { return decodedFrames() + encodedFrames(); }

    [[nodiscard]] int64_t bufferedDurationUs() const noexcept {
        return static_cast<int64_t>(bufferedFrames()) * config::audio::kFrameDurationUs;
    }

    [[nodiscard]] int64_t decodedDurationUs() const noexcept {
        return static_cast<int64_t>(decodedFrames()) * config::audio::kFrameDurationUs;
    }

    [[nodiscard]] bool hasPendingDecode() const noexcept {
        return decodeWorkPending_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool needsPLC() const noexcept {
        if (!isActive()) return false;
        StreamState s = state_.load(std::memory_order_acquire);
        if (s == StreamState::Paused) return false;
        if (s != StreamState::Playing && s != StreamState::Underrun) return false;
        // Use decoded-only: PLC fills the decoded queue for the audio callback.
        // Encoded frames pending decode don't help the callback right now.
        return decodedDurationUs() < config::audio::kPLCThresholdUs;
    }

    void setGain(float gain) noexcept {
        targetGain_.store(std::clamp(gain, config::mix::kMinGain, config::mix::kMaxGain),
                          std::memory_order_relaxed);
    }

    [[nodiscard]] float targetGain() const noexcept {
        return targetGain_.load(std::memory_order_relaxed);
    }

    void setBufferTarget(int64_t targetUs) noexcept {
        bufferTargetUs_.store(
            std::clamp(targetUs, config::jitter::kMinBufferUs, config::jitter::kMaxBufferUs),
            std::memory_order_relaxed);
    }

    void setCatchupPolicy(CatchupPolicy p) noexcept {
        catchupPolicy_.store(p, std::memory_order_relaxed);
    }

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
    void requestEncodedClear() noexcept {
        encodedClearRequested_.store(true, std::memory_order_release);
    }

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

    void markUnderrun() noexcept {
        StreamState prev = state_.exchange(StreamState::Underrun, std::memory_order_acq_rel);
        if (prev != StreamState::Underrun) {
            metrics_.underruns.fetch_add(1, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] int64_t lastRenderedPtsUs() const noexcept {
        return lastRenderedPtsUs_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] const StreamMetrics& metrics() const noexcept { return metrics_; }

    [[nodiscard]] bool isDecodedPoolUnderPressure() const noexcept {
        return pool_.isUnderPressure();
    }

    [[nodiscard]] bool isEncodedPoolUnderPressure() const noexcept {
        return encodedPool_.isUnderPressure();
    }

    void resetMetrics() noexcept { metrics_.reset(); }

    void reset() noexcept {
        deactivate();
        metrics_.reset();
    }

private:
    // LOCK-FREE: Called from audio callback thread only.
    // Discards up to maxFrames decoded frames, saving tail for crossfade splice.
    size_t drainDecodedFrames(size_t maxFrames) noexcept {
        if (maxFrames == 0) return 0;

        // Only crossfade at the start of a burst drain sequence.
        // drainActive_ persists across callbacks (unlike drainFadePending_ which
        // is consumed each callback by applyDrainCrossfade). This prevents
        // repeated 5ms crossfades on every callback during sustained drain —
        // only the first drain event of each burst gets a crossfade splice.
        bool needsFade = !drainActive_;

        size_t drained = 0;

        // Discard partial frame first if present
        if (partialToken_) {
            if (needsFade) {
                DecodedAudioFrame* frame = partialToken_.get();
                saveDrainFadeTail(frame->samples.data(), frame->sampleCount);
            }
            partialToken_.release();
            partialOffset_ = 0;
            drained++;
        }

        // Drain from decoded queue. Save crossfade tail from the last frame
        // BEFORE releasing the token — frame memory returns to pool on release.
        while (drained < maxFrames) {
            auto token = decodedQueue_.pop();
            if (!token) break;
            if (needsFade) {
                DecodedAudioFrame* frame = token.get();
                saveDrainFadeTail(frame->samples.data(), frame->sampleCount);
            }
            drained++;
        }

        if (drained > 0) {
            if (needsFade) drainFadePending_ = true;
            drainActive_ = true;
            metrics_.framesDrained.fetch_add(drained, std::memory_order_relaxed);
        }
        return drained;
    }

    // Save last kFadeSamples from a frame's PCM for crossfade splice
    void saveDrainFadeTail(const float* samples, size_t sampleCount) noexcept {
        constexpr size_t kFade = config::crossfade::kFadeSamples;
        if (sampleCount >= kFade) {
            std::memcpy(drainFadeBuffer_.data(), samples + sampleCount - kFade,
                        kFade * sizeof(float));
        } else {
            size_t pad = kFade - sampleCount;
            std::memset(drainFadeBuffer_.data(), 0, pad * sizeof(float));
            std::memcpy(drainFadeBuffer_.data() + pad, samples,
                        sampleCount * sizeof(float));
        }
    }

    // Blend saved tail (fade-out) into start of new output (fade-in) after drain.
    // LOCK-FREE: audio callback thread only.
    void applyDrainCrossfade(float* output, size_t sampleCount) noexcept {
        if (!drainFadePending_) return;
        drainFadePending_ = false;

        size_t fadeSamples = std::min(config::crossfade::kFadeSamples, sampleCount);
        if (fadeSamples == 0) return;

        float fadeStep = 1.0f / static_cast<float>(fadeSamples);
        for (size_t i = 0; i < fadeSamples; ++i) {
            float fadeOut = 1.0f - static_cast<float>(i) * fadeStep;
            float fadeIn = static_cast<float>(i) * fadeStep;
            output[i] = drainFadeBuffer_[i] * fadeOut + output[i] * fadeIn;
        }
    }

    // Save tail of concealed audio for PLC→real crossfade splice.
    // LOCK-FREE: audio callback thread only.
    void savePlcFadeTail(const float* buffer, size_t sampleCount) noexcept {
        constexpr size_t kFade = config::crossfade::kFadeSamples;
        if (sampleCount >= kFade) {
            std::memcpy(plcFadeBuffer_.data(), buffer + sampleCount - kFade,
                        kFade * sizeof(float));
        } else if (sampleCount > 0) {
            size_t pad = kFade - sampleCount;
            std::memset(plcFadeBuffer_.data(), 0, pad * sizeof(float));
            std::memcpy(plcFadeBuffer_.data() + pad, buffer,
                        sampleCount * sizeof(float));
        } else {
            return;
        }
        plcFadePending_ = true;
    }

    // Blend saved PLC tail (fade-out) into start of real audio (fade-in).
    // LOCK-FREE: audio callback thread only.
    void applyPlcCrossfade(float* output, size_t sampleCount) noexcept {
        if (!plcFadePending_) return;
        plcFadePending_ = false;

        size_t fadeSamples = std::min(config::crossfade::kFadeSamples, sampleCount);
        if (fadeSamples == 0) return;

        float fadeStep = 1.0f / static_cast<float>(fadeSamples);
        for (size_t i = 0; i < fadeSamples; ++i) {
            float fadeOut = 1.0f - static_cast<float>(i) * fadeStep;
            float fadeIn = static_cast<float>(i) * fadeStep;
            output[i] = plcFadeBuffer_[i] * fadeOut + output[i] * fadeIn;
        }
    }

    [[nodiscard]] bool decodeFrameWithToken(const uint8_t* data, size_t size,
                                              int64_t ptsUs, int64_t durationUs,
                                              DecodedAudioToken token) noexcept {
        if (!decoder_) return false;

        // Reset decoder if it has accumulated too many consecutive errors
        if (decoder_->consecutiveErrors() >= config::audio::kMaxConsecutiveDecodeErrors) {
            metrics_.decoderResets.fetch_add(1, std::memory_order_relaxed);
            decoder_->reset();
        }

        DecodedAudioFrame* frame = token.get();
        int decoded = decoder_->decode(data, size, frame->samples.data(), config::audio::kFrameSamples);
        if (decoded <= 0) {
            metrics_.decodeErrors.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        consecutivePLCFrames_ = 0;

        frame->sampleCount = static_cast<uint32_t>(decoded) * static_cast<uint32_t>(config::audio::kChannels);
        frame->ptsUs = ptsUs;
        frame->durationUs = durationUs;
        frame->isConcealed = false;

        lastDecodedPtsUs_.store(ptsUs, std::memory_order_relaxed);

        if (!decodedQueue_.push(std::move(token))) {
            metrics_.framesDropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        StreamState currentState = state_.load(std::memory_order_acquire);
        if (currentState == StreamState::Buffering || currentState == StreamState::Underrun) {
            int64_t targetUs = bufferTargetUs_.load(std::memory_order_relaxed);
            // Use decoded-only duration: audio callback can only read decoded frames,
            // so transition to Playing only when enough decoded data is available.
            int64_t decodedUs = decodedDurationUs();
            // After underrun, require target + one frame before resuming. The extra
            // frame of hysteresis prevents oscillation when burst arrival rate
            // exactly matches the target (e.g., 3 frames/80ms with 60ms target).
            // readSamples() drains concurrently during Underrun, so the margin
            // compensates for frames consumed between threshold check and playback.
            // For initial buffering, use minimum threshold for faster startup.
            int64_t thresholdUs = (currentState == StreamState::Underrun)
                                  ? targetUs + config::audio::kFrameDurationUs
                                  : config::jitter::kMinBufferUs;
            if (decodedUs >= thresholdUs) {
                state_.store(StreamState::Playing, std::memory_order_release);
                MEDIA_LOG_D("AudioDecodeChannel: now Playing (decoded=%lldus, target=%lldus)",
                            static_cast<long long>(decodedUs),
                            static_cast<long long>(targetUs));
            }
        }

        return true;
    }

    DecodedAudioPool& pool_;

    PendingAudioPool encodedPool_;
    PendingAudioQueue encodedQueue_;

    DecodedAudioQueue decodedQueue_;
    std::unique_ptr<OpusDecoderAdapter> decoder_;

    std::mutex decoderMtx_;

    alignas(config::kCacheLineSize) std::atomic<StreamState> state_{StreamState::Inactive};

    alignas(config::kCacheLineSize) std::atomic<int64_t> lastPtsUs_{config::sentinel::kNoTimestamp};
    std::atomic<int64_t> lastDecodedPtsUs_{0};
    std::atomic<int64_t> lastPushTimeUs_{0};  // wall-clock time of last pushEncodedFrame call
    alignas(config::kCacheLineSize) std::atomic<int64_t> lastRenderedPtsUs_{0};
    std::atomic<bool> decodeWorkPending_{false};
    std::atomic<bool> needsDecoderReset_{false};
    // Deferred encoded queue clear requested by deactivate(). Serviced by decode thread
    // via serviceDeferredClear() to avoid SPSC contract violation — deactivate() runs on
    // the JS thread while the decode thread is the sole consumer of encodedQueue_.
    std::atomic<bool> encodedClearRequested_{false};
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
    std::atomic<CatchupPolicy> catchupPolicy_{CatchupPolicy::PlayThrough};
    std::atomic<float> playbackRate_{config::playbackrate::kDefaultRate};

    StreamMetrics metrics_;

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

    // Fast-drain crossfade state — audio callback thread only, no synchronization
    alignas(config::kSimdAlignment) std::array<float, config::crossfade::kFadeSamples> drainFadeBuffer_{};
    bool drainFadePending_{false};  // pending crossfade for current callback
    bool drainActive_{false};       // sustained drain across callbacks (reset when excess ≤ 0)

    // PLC→real crossfade state — audio callback thread only, no synchronization
    alignas(config::kSimdAlignment) std::array<float, config::crossfade::kFadeSamples> plcFadeBuffer_{};
    bool plcFadePending_{false};        // pending crossfade for current callback
    bool lastFrameWasConcealed_{false}; // concealment state of last consumed frame

    // Fast-path / resampled-path hysteresis — prevents oscillation when
    // combined ratio hovers near unity (common on Bluetooth SCO).
    uint8_t fastPathHoldCount_{0};
    bool lastWasFastPath_{true};

    // Consecutive PLC frame counter — decode thread only (under decoderMtx_)
    uint32_t consecutivePLCFrames_{0};

    // A/V sync coordinator — nullable, set when video pipeline is active.
    // Audio callback reports render PTS via atomic stores (safe from RT thread).
    std::atomic<AVSyncCoordinator*> syncCoordinator_{nullptr};
};

}  // namespace media
