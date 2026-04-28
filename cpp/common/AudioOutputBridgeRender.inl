// Lock-free RT-safe render core for AudioOutputBridgeBase: invoked from the
// platform audio callback. Handles state checks, jitter tracking, optional
// SR resampling (when hardware doesn't grant the configured rate), and frame
// accounting.
//
// Include-only-from: AudioOutputBridgeBase.h. Out-of-class template member
// definition; included after the class body so the full type is visible.
#pragma once

namespace media {

template<typename Derived>
size_t AudioOutputBridgeBase<Derived>::renderAudioCore(float* output, size_t frameCount) noexcept {
    if (UNLIKELY(!output || frameCount == 0)) return 0;

    const size_t totalSamples = frameCount * static_cast<size_t>(baseConfig_.channelCount);

    uint8_t state = callbackState_.load(std::memory_order_acquire);
    if (UNLIKELY(interruptedFlag_ && interruptedFlag_->load(std::memory_order_acquire))) {
        std::memset(output, 0, totalSamples * sizeof(float));
        return frameCount;
    }
    if (UNLIKELY(state == kStateWarmUp)) {
        std::memset(output, 0, totalSamples * sizeof(float));
        return frameCount;
    }
    if (UNLIKELY(state != kStateActive)) {
        std::memset(output, 0, totalSamples * sizeof(float));
        return 0;
    }

    // Callback jitter tracking
    {
        int64_t now = nowUs();
        int64_t prev = lastCallbackTimeUs_.load(std::memory_order_relaxed);
        if (prev > 0) {
            int64_t expectedIntervalUs = static_cast<int64_t>(frameCount) * 1000000LL
                / baseConfig_.sampleRate;
            int64_t actualInterval = now - prev;
            int64_t deviation = actualInterval > expectedIntervalUs
                ? actualInterval - expectedIntervalUs
                : expectedIntervalUs - actualInterval;
            int64_t curPeak = callbackJitterUs_.load(std::memory_order_relaxed);
            if (deviation > curPeak) {
                callbackJitterUs_.store(deviation, std::memory_order_relaxed);
            }
        }
        lastCallbackTimeUs_.store(now, std::memory_order_relaxed);
    }

    // Dispatch through callback with optional resampling
    size_t written = 0;
    if (LIKELY(static_cast<bool>(callback_))) {
        if (LIKELY(playbackResampler_.isPassthrough())) {
            written = callback_(output, frameCount);
        } else {
            int32_t hwRate = grantedSampleRate_.load(std::memory_order_relaxed);
            if (UNLIKELY(hwRate <= 0)) hwRate = baseConfig_.sampleRate;

            size_t srcFrames = (frameCount * static_cast<size_t>(baseConfig_.sampleRate)
                                + static_cast<size_t>(hwRate) - 1)
                               / static_cast<size_t>(hwRate);
            srcFrames += 1;  // Speex filter delay guard

            if (UNLIKELY(srcFrames > resampleSrcBuffer_.size())) {
                srcFrames = resampleSrcBuffer_.size();
            }

            written = callback_(resampleSrcBuffer_.data(), srcFrames);

            if (LIKELY(written > 0)) {
                size_t resampledCount = playbackResampler_.process(
                    output, resampleSrcBuffer_.data(), written);
                written = std::min(resampledCount, frameCount);
            }
        }
    }

    if (LIKELY(written > 0)) {
        framesWritten_.fetch_add(static_cast<int64_t>(written), std::memory_order_relaxed);
    }

    if (written < frameCount) {
        size_t remaining = (frameCount - written) * static_cast<size_t>(baseConfig_.channelCount);
        std::memset(output + written * static_cast<size_t>(baseConfig_.channelCount), 0,
                    remaining * sizeof(float));
    }

    return written;
}

}  // namespace media
