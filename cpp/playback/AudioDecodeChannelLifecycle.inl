// State-mutation member functions of AudioDecodeChannel: activate, deactivate,
// pushEncodedFrame. All are driven from the JS thread (with deferred-clear
// flags that the decode thread / audio callback service later).
//
// Include-only-from: AudioDecodeChannel.h. Out-of-class definitions; `inline`
// satisfies ODR across multiple translation units that include the header.
#pragma once

namespace media {

inline void AudioDecodeChannel::activate() noexcept {
    std::lock_guard<std::mutex> lk(decoderMtx_);

    // Transition to Inactive first so concurrent readSamples() exits early
    state_.store(StreamState::Inactive, std::memory_order_release);

    lastPtsUs_.store(config::sentinel::kNoTimestamp, std::memory_order_relaxed);
    lastDecodedPtsUs_.store(0, std::memory_order_relaxed);
    lastPushTimeUs_.store(0, std::memory_order_relaxed);
    lastRenderedPtsUs_.store(0, std::memory_order_relaxed);
    targetGain_.store(config::mix::kDefaultGain, std::memory_order_relaxed);
    bufferTargetUs_.store(config::jitter::kDefaultBufferUs, std::memory_order_relaxed);
    arrivalConfidence_.store(0.0f, std::memory_order_relaxed);

    // Do NOT clear encQueue_ directly — serviceDeferredClear() on the decode
    // thread may be mid-clear(). Only the decode thread touches the consumer
    // side. Request a deferred clear; the decode thread services it next cycle.
    encQueue_.requestClear();

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

    // Do NOT call catchup_.reset(), resampler_.reset(), or reset
    // driftBufferLeftover_ here — these are audio-callback-only state
    // and would race with readSamples(). The decodedClearRequested_
    // handler in readSamples() resets them from the correct thread.

    // Clear any pending reset flag from previous session
    needsDecoderReset_.store(false, std::memory_order_relaxed);

    // Reset ring offset tracking for new epoch
    lastDecodedAbsOffset_.store(0, std::memory_order_relaxed);

    if (decoder_) decoder_->reset();
    consecutivePLCFrames_ = 0;

    state_.store(StreamState::Buffering, std::memory_order_release);

    MEDIA_LOG_D("AudioDecodeChannel: activated");
}

// Non-blocking deactivate. Thread safety guaranteed by release/acquire ordering:
// 1. state_ set to Inactive causes readSamples() and pushEncodedFrame() to exit early
// 2. partialClearRequested_ signals audio callback to release partial frame
// 3. encQueue_.requestClear() signals decode thread to clear encoded queue
inline void AudioDecodeChannel::deactivate() noexcept {
    std::lock_guard<std::mutex> lk(decoderMtx_);

    if (state_.load(std::memory_order_relaxed) == StreamState::Inactive) return;

    // Signal audio callback to stop reading - release ensures visibility
    state_.store(StreamState::Inactive, std::memory_order_release);

    // Request partial frame cleanup - audio callback will process on next run
    partialClearRequested_.store(true, std::memory_order_release);

    // Do NOT clear encQueue_ directly — the decode thread may be mid-pop() in
    // processPendingDecode() (which runs outside decoderMtx_). Defer to the
    // decode thread via requestClear(); it services the request each cycle.
    // Do NOT clear decodedQueue_ here either — the audio callback may still
    // be mid-pop(). activate() defers that via decodedClearRequested_.
    encQueue_.requestClear();

    if (decoder_) decoder_->reset();

    MEDIA_LOG_D("AudioDecodeChannel: deactivated");
}

inline bool AudioDecodeChannel::pushEncodedFrame(const RawAudioFrame& frame) noexcept {
    if (!isActive()) return false;

    if (frame.size == 0 || frame.size > config::audio::kMaxEncodedFrameSize) {
        metrics_.framesDropped.fetch_add(1, std::memory_order_relaxed);
        metrics_.oversizedFrameDrops.fetch_add(1, std::memory_order_relaxed);
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
            if (gapUs > config::gap::kBucket500msUs) {
                metrics_.gapsOver500ms.fetch_add(1, std::memory_order_relaxed);
                metrics_.gapsOver100ms.fetch_add(1, std::memory_order_relaxed);
                metrics_.gapsOver50ms.fetch_add(1, std::memory_order_relaxed);
            } else if (gapUs > config::gap::kBucket100msUs) {
                metrics_.gapsOver100ms.fetch_add(1, std::memory_order_relaxed);
                metrics_.gapsOver50ms.fetch_add(1, std::memory_order_relaxed);
            } else if (gapUs > config::gap::kBucket50msUs) {
                metrics_.gapsOver50ms.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    // DTX frames are valid Opus packets that decode to comfort noise.
    // Pass them through to the decoder - do not skip them.

    int64_t buffered = bufferedDurationUs();
    if (buffered >= config::audio::kMaxBufferedUs) {
        uint64_t drops = metrics_.framesDropped.fetch_add(1, std::memory_order_relaxed) + 1;
        metrics_.bufferFullDrops.fetch_add(1, std::memory_order_relaxed);
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

    auto token = encQueue_.acquire();
    if (!token) {
        metrics_.framesDropped.fetch_add(1, std::memory_order_relaxed);
        metrics_.encodedPoolExhaustionDrops.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    PendingAudioFrame* pending = token.get();
    pending->absOffset = frame.absOffset;
    pending->size = frame.size;
    pending->ptsUs = frame.timestampUs;
    pending->durationUs = frame.durationUs;

    if (!encQueue_.push(std::move(token))) {
        metrics_.framesDropped.fetch_add(1, std::memory_order_relaxed);
        metrics_.encodedPushFailDrops.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    metrics_.framesReceived.fetch_add(1, std::memory_order_relaxed);
    return true;
}

}  // namespace media
