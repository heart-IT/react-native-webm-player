// Decode-thread orchestration: processPendingDecode (outer loop), generatePLC
// (synthetic-frame generation when buffer underruns), and decodeFrameWithToken
// (per-packet Opus decode + FEC + Buffering→Playing transition).
//
// All run under decoderMtx_ (try-lock — never block the decode thread on the
// JS thread). Member functions of AudioDecodeChannel; class declaration in
// AudioDecodeChannel.h.
#pragma once

namespace media {

inline bool AudioDecodeChannel::processPendingDecode() noexcept {
    if (!encQueue_.hasPendingWork()) return false;
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

        auto token = encQueue_.pop();
        if (!token) {
            // No encoded frames — release the pre-acquired token back to pool.
            // Token destructor handles this automatically via RAII.
            break;
        }

        PendingAudioFrame* pending = token.get();

        if (decodeFrameWithToken(pending->absOffset, pending->size,
                                 pending->ptsUs, pending->durationUs,
                                 std::move(decodedToken))) {
            didWork = true;
        }
    }

    // Only clear the work-pending flag if all work is truly done.
    // Two conditions break the decode loop with work remaining:
    //   1. Pool exhaustion — no tokens available
    //   2. Cap — enough decoded frames
    // In both cases, keep the flag set so DecodeThread re-enters on the
    // next cycle (~500us) when the audio callback frees pool entries.
    bool workRemains = encQueue_.count() > 0;
    if (!workRemains) {
        encQueue_.markWorkConsumed();
    }
    return didWork;
}

inline bool AudioDecodeChannel::generatePLC() noexcept {
    // try_lock avoids priority inversion (see processPendingDecode comment).
    std::unique_lock<std::mutex> lk(decoderMtx_, std::try_to_lock);
    if (!lk.owns_lock()) return false;

    if (!decoder_ || !isActive()) return false;

    // After kMaxConsecutivePLC frames, Opus PLC quality degrades badly.
    // Reset decoder and return false to let the mixer's CNG handle it.
    // Bump decoderResets so triage sees this reset path; the counter is left
    // non-zero so subsequent cycles keep returning false here — letting the
    // audioRecv/audioOutput-delta watchdog surface the stuck state rather
    // than hiding it behind continuously regenerated PLC frames.
    if (consecutivePLCFrames_ >= config::plc::kMaxConsecutivePLC) {
        decoder_->reset();
        metrics_.decoderResets.fetch_add(1, std::memory_order_relaxed);
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

inline bool AudioDecodeChannel::decodeFrameWithToken(long long absOffset, size_t size,
                                                      int64_t ptsUs, int64_t durationUs,
                                                      DecodedAudioToken token) noexcept {
    if (!decoder_) return false;

    // Reset decoder if it has accumulated too many consecutive errors
    if (decoder_->consecutiveErrors() >= config::audio::kMaxConsecutiveDecodeErrors) {
        metrics_.decoderResets.fetch_add(1, std::memory_order_relaxed);
        decoder_->reset();
    }

    // Read encoded data from ring buffer (zero-copy when not wrapping). Fall back
    // to the member scratch buffer on wrap — avoids allocating 1.2KB of stack per call.
    const uint8_t* data = nullptr;
    auto* ring = ring_.load(std::memory_order_acquire);
    if (ring) {
        data = ring->dataAt(absOffset, static_cast<long long>(size));
        if (!data) {
            if (ring->readAt(absOffset, static_cast<long>(size), decodeLinearBuf_.data()) == 0) {
                data = decodeLinearBuf_.data();
            }
        }
    }
    if (!data) return false;

    // Opus in-band FEC: when we detect a single-frame PTS gap, this packet
    // may carry redundant data for the lost frame.  Decode FEC first (recovering
    // the missing frame), then decode this packet normally.  FEC quality is lower
    // than the original but dramatically better than PLC's synthetic output.
    int64_t lastPts = lastDecodedPtsUs_.load(std::memory_order_relaxed);
    int64_t gap = ptsUs - lastPts;
    bool fecRecovered = false;
    if (lastPts > 0 && gap > config::audio::kFrameDurationUs &&
        gap <= 2 * config::audio::kFrameDurationUs) {
        auto fecToken = pool_.acquire();
        if (fecToken) {
            DecodedAudioFrame* fecFrame = fecToken.get();
            int fecDecoded = decoder_->decodeFEC(data, size,
                                                  fecFrame->samples.data(),
                                                  config::audio::kFrameSamples);
            if (fecDecoded > 0) {
                fecFrame->sampleCount = static_cast<uint32_t>(fecDecoded) *
                                        static_cast<uint32_t>(config::audio::kChannels);
                fecFrame->ptsUs = lastPts + config::audio::kFrameDurationUs;
                fecFrame->durationUs = config::audio::kFrameDurationUs;
                fecFrame->isConcealed = true;
                consecutivePLCFrames_ = 0;
                lastDecodedPtsUs_.store(fecFrame->ptsUs, std::memory_order_relaxed);
                if (decodedQueue_.push(std::move(fecToken))) {
                    // Only count playable FEC frames — pre-push counting inflated
                    // fecFrames during back-pressure (push failures). Mirrors the
                    // success-after-push pattern used in the normal decode branch.
                    metrics_.fecFrames.fetch_add(1, std::memory_order_relaxed);
                } else {
                    metrics_.framesDropped.fetch_add(1, std::memory_order_relaxed);
                    metrics_.decodedPushFailDrops.fetch_add(1, std::memory_order_relaxed);
                }
                fecRecovered = true;
            }
            // If FEC decode fails, token is released by RAII — fall through to
            // normal decode.  Opus decoder state is still valid for the next call.
        }
    }

    DecodedAudioFrame* frame = token.get();
    int64_t t0 = nowUs();
    int decoded = decoder_->decode(data, size, frame->samples.data(), config::audio::kFrameSamples);
    int64_t elapsed = nowUs() - t0;
    if (decoded <= 0) {
        metrics_.decodeErrors.fetch_add(1, std::memory_order_relaxed);
        metrics_.lastDecodeError.store(decoder_->lastError(), std::memory_order_relaxed);
        return fecRecovered;
    }
    auto prev = metrics_.decodeLatencyUs.load(std::memory_order_relaxed);
    auto smoothed = prev == 0 ? elapsed : prev + (elapsed - prev) / 8;
    metrics_.decodeLatencyUs.store(smoothed, std::memory_order_relaxed);

    consecutivePLCFrames_ = 0;

    frame->sampleCount = static_cast<uint32_t>(decoded) * static_cast<uint32_t>(config::audio::kChannels);
    frame->ptsUs = ptsUs;
    frame->durationUs = durationUs;
    frame->isConcealed = false;

    lastDecodedPtsUs_.store(ptsUs, std::memory_order_relaxed);
    lastDecodedAbsOffset_.store(absOffset, std::memory_order_relaxed);

    if (!decodedQueue_.push(std::move(token))) {
        metrics_.framesDropped.fetch_add(1, std::memory_order_relaxed);
        metrics_.decodedPushFailDrops.fetch_add(1, std::memory_order_relaxed);
        return fecRecovered;
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
        bool speculative = arrivalConfidence_.load(std::memory_order_relaxed)
                           >= config::speculative::kConfidenceThreshold;
        int64_t thresholdUs;
        if (currentState == StreamState::Underrun) {
            // After underrun, require hysteresis to prevent oscillation.
            // Speculative: 2 frames (40ms). Conservative: target + 1 frame.
            thresholdUs = speculative
                ? 2 * config::audio::kFrameDurationUs
                : targetUs + config::audio::kFrameDurationUs;
        } else {
            // Initial buffering: speculative starts at 1 frame (20ms).
            thresholdUs = speculative
                ? config::audio::kFrameDurationUs
                : config::jitter::kMinBufferUs;
        }
        if (decodedUs >= thresholdUs) {
            state_.store(StreamState::Playing, std::memory_order_release);
            MEDIA_LOG_D("AudioDecodeChannel: now Playing (decoded=%lldus, target=%lldus)",
                        static_cast<long long>(decodedUs),
                        static_cast<long long>(targetUs));
        }
    }

    return true;
}

}  // namespace media
