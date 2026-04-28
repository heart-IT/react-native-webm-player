// Lock-free RT-thread audio output path: readSamples (entry point + state
// machine), collectDecodedFrames (frame popping + silence-skip + PLC
// transition detection), finalizeOutput (crossfades + metrics + A/V sync),
// readSamplesNoDrift / readSamplesWithDrift (fast path + Speex resampler
// path), drainDecodedFrames (fast-drain), isFrameSilence (helper).
//
// All audio-callback-thread-only — no mutexes, no allocations, no I/O.
// Member functions of AudioDecodeChannel; class declaration in
// AudioDecodeChannel.h.
#pragma once

namespace media {

inline size_t AudioDecodeChannel::readSamples(float* output, size_t maxSamples) noexcept {
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
        drain_.reset();
        plc_.reset();
        fastPathHoldCount_ = 0;
        lastWasFastPath_ = true;
        speculativeHoldCount_ = 0;
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
        plc_.reset();
        partialClearRequested_.store(false, std::memory_order_release);
    }

    // Fast-drain: when buffer depth far exceeds target, discard decoded
    // frames to snap latency back toward target. Each discarded frame frees
    // a pool token so the decode thread refills on its next cycle (~500us).
    // A single 5ms crossfade splice at the burst start prevents audible clicks.
    {
        auto decision = drainPolicy_.evaluate(
            bufferedDurationUs(),
            bufferTargetUs_.load(std::memory_order_relaxed),
            decodedFrames(),
            static_cast<bool>(partialToken_));
        if (UNLIKELY(decision.framesToDrain > 0)) {
            drainDecodedFrames(decision.framesToDrain);
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
        } else if (decision.burstEnded) {
            // Burst drain complete — reset for next burst
            drain_.markIdle();
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

inline AudioDecodeChannel::CollectResult
AudioDecodeChannel::collectDecodedFrames(float* dest, size_t maxSamples) noexcept {
    size_t written = 0;

    // Consume any remaining samples from partial frame
    if (partialToken_ && partialOffset_ < partialToken_->sampleCount) {
        DecodedAudioFrame* frame = partialToken_.get();
        size_t remaining = frame->sampleCount - partialOffset_;
        size_t toCopy = std::min(maxSamples, remaining);

        std::memcpy(dest, frame->samples.data() + partialOffset_, toCopy * sizeof(float));
        written += toCopy;
        partialOffset_ += toCopy;

        if (partialOffset_ >= frame->sampleCount) {
            partialToken_.release();
            partialOffset_ = 0;
        }
    }

    int64_t lastRenderPts = 0;
    size_t silenceSkips = 0;

    while (LIKELY(written < maxSamples)) {
        auto token = decodedQueue_.pop();
        if (!token) break;

        DecodedAudioFrame* frame = token.get();

        // Silence-skip catchup: if buffer has moderate excess and this frame
        // is silence, skip it to instantly reduce latency by one frame (20ms).
        if (UNLIKELY(silenceSkips < config::silenceskip::kMaxSkipsPerCallback &&
                     decodedDurationUs() > bufferTargetUs_.load(std::memory_order_relaxed) +
                                            config::silenceskip::kExcessThresholdUs &&
                     isFrameSilence(frame))) {
            if (frame->ptsUs > 0) lastRenderPts = frame->ptsUs;
            ++silenceSkips;
            continue;
        }

        // Detect PLC→real transition: save tail of concealed output for crossfade
        if (UNLIKELY(plc_.wasLastFrameConcealed() && !frame->isConcealed)) {
            plc_.saveTail(dest, written);
        }
        plc_.setLastFrameConcealed(frame->isConcealed);

        if (!frame->isConcealed && frame->ptsUs > 0) {
            lastRenderPts = frame->ptsUs;
        }

        size_t toCopy = std::min(maxSamples - written, static_cast<size_t>(frame->sampleCount));
        std::memcpy(dest + written, frame->samples.data(), toCopy * sizeof(float));
        written += toCopy;

        if (UNLIKELY(toCopy < frame->sampleCount)) {
            partialToken_ = std::move(token);
            partialOffset_ = toCopy;
            break;
        }
    }

    return {written, lastRenderPts, silenceSkips};
}

inline void AudioDecodeChannel::finalizeOutput(float* output, size_t written,
                                                int64_t lastRenderPts, size_t silenceSkips) noexcept {
    if (UNLIKELY(silenceSkips > 0)) {
        metrics_.silenceSkipFrames.fetch_add(silenceSkips, std::memory_order_relaxed);
    }

    speculativeHoldCount_ = 0;
    drain_.applyCrossfade(output, written);
    plc_.applyCrossfade(output, written);
    metrics_.samplesOutput.fetch_add(written, std::memory_order_relaxed);

    if (lastRenderPts > 0) {
        lastRenderedPtsUs_.store(lastRenderPts, std::memory_order_relaxed);
        auto* sync = syncCoordinator_.load(std::memory_order_acquire);
        if (sync) {
            sync->onAudioRender(lastRenderPts, nowUs());
        }
    }
}

inline size_t AudioDecodeChannel::readSamplesNoDrift(float* output, size_t maxSamples) noexcept {
    auto result = collectDecodedFrames(output, maxSamples);
    if (result.samplesCollected > 0) {
        finalizeOutput(output, result.samplesCollected, result.lastRenderPts, result.silenceSkips);
    }
    return result.samplesCollected;
}

inline size_t AudioDecodeChannel::readSamplesWithDrift(float* output, size_t maxSamples, float ratio) noexcept {
    if (UNLIKELY(!std::isfinite(ratio) || ratio <= 0.0f || ratio > SpeexDriftResampler::kMaxCombinedRatio)) {
        ratio = 1.0f;
    }

    resampler_.setRatio(ratio);

    // Calculate how many input samples we need to produce maxSamples output
    size_t inputNeeded = static_cast<size_t>(static_cast<float>(maxSamples) * ratio) + 4;
    inputNeeded = std::min(inputNeeded, driftBuffer_.size());

    // Collect input samples into drift buffer (preserving leftover from previous call)
    size_t leftover = driftBufferLeftover_.load(std::memory_order_relaxed);
    leftover = std::min(leftover, driftBuffer_.size());
    float* driftBuf = driftBuffer_.data();

    // Skip collection when leftover already satisfies inputNeeded (avoids
    // size_t underflow in inputNeeded - leftover after ratio drops).
    CollectResult result{0, 0, 0};
    if (leftover < inputNeeded) {
        result = collectDecodedFrames(driftBuf + leftover, inputNeeded - leftover);
    }
    size_t inputCollected = leftover + result.samplesCollected;

    // Apply Speex resampling
    size_t outputWritten = 0;
    if (inputCollected > 0) {
        size_t inputConsumed = 0;
        outputWritten = resampler_.process(driftBuf, inputCollected,
                                           output, maxSamples,
                                           inputConsumed);

        if (inputConsumed < inputCollected) {
            size_t remaining = inputCollected - inputConsumed;
            std::memmove(driftBuf, driftBuf + inputConsumed, remaining * sizeof(float));
            driftBufferLeftover_.store(remaining, std::memory_order_relaxed);
        } else {
            driftBufferLeftover_.store(0, std::memory_order_relaxed);
        }
    }

    if (outputWritten > 0) {
        finalizeOutput(output, outputWritten, result.lastRenderPts, result.silenceSkips);
    }

    return outputWritten;
}

inline size_t AudioDecodeChannel::drainDecodedFrames(size_t maxFrames) noexcept {
    if (maxFrames == 0) return 0;

    // Only crossfade at the start of a burst drain sequence. drain_.active()
    // persists across callbacks; the pending flag is consumed each callback
    // by applyCrossfade(). This prevents repeated 5ms crossfades during
    // sustained drain — only the first drain event of each burst splices.
    bool needsFade = !drain_.active();

    size_t drained = 0;

    // Discard partial frame first if present
    if (partialToken_) {
        if (needsFade) {
            DecodedAudioFrame* frame = partialToken_.get();
            drain_.saveTail(frame->samples.data(), frame->sampleCount);
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
            drain_.saveTail(frame->samples.data(), frame->sampleCount);
        }
        drained++;
    }

    if (drained > 0) {
        if (needsFade) drain_.markPending();
        drain_.markActive();
        metrics_.framesDrained.fetch_add(drained, std::memory_order_relaxed);
    }
    return drained;
}

// RT-safe: single pass over samples, no branches except the early exit.
inline bool AudioDecodeChannel::isFrameSilence(const DecodedAudioFrame* frame) noexcept {
    constexpr float kThreshold = config::silenceskip::kSilencePeakThreshold;
    const float* samples = frame->samples.data();
    size_t count = frame->sampleCount;
    for (size_t i = 0; i < count; ++i) {
        float v = samples[i];
        if (v > kThreshold || v < -kThreshold) return false;
    }
    return true;
}

}  // namespace media
