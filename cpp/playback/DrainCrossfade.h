// Fast-drain crossfade — RT-thread only.
//
// When the audio buffer accumulates excess depth (above target + policy
// threshold), the RT thread discards decoded frames to snap latency back
// toward target. A single 5ms equal-power crossfade splice at the START of
// each drain burst prevents audible clicks; subsequent drains within the
// same burst skip the fade.
//
// State machine (all on the audio callback thread):
//   active_  — true between burst start and "excess <= 0" reset.
//              The caller queries active() to decide whether the next
//              drain is a fresh burst (needs fade) or a continuation.
//   pending_ — fade is queued for the next applyCrossfade() call;
//              consumed (cleared) by applyCrossfade().
//
// Caller flow per drain cycle:
//   bool fresh = !drain.active();
//   for each frame discarded:
//       if (fresh) drain.saveTail(frame->samples, frame->sampleCount);
//   if (drained > 0) {
//       if (fresh) drain.markPending();
//       drain.markActive();
//   }
// And when buffer excess drops back to 0:
//   drain.markIdle();
#pragma once

#include "common/CompilerHints.h"
#include "common/MediaConfig.h"

#include <array>
#include <cstring>

namespace media {

class DrainCrossfade {
public:
    [[nodiscard]] bool active() const noexcept { return active_; }
    void markActive() noexcept { active_ = true; }
    void markIdle() noexcept { active_ = false; }
    void markPending() noexcept { pending_ = true; }

    // Save the last kFadeSamples of a frame about to be discarded.
    // Caller must only invoke this on the first burst's drained frames
    // (otherwise the fade tail will be repeatedly overwritten — see header
    // doc state machine). RT-safe.
    HOT_FUNCTION
    void saveTail(const float* samples, size_t sampleCount) noexcept {
        constexpr size_t kFade = config::crossfade::kFadeSamples;
        if (sampleCount >= kFade) {
            std::memcpy(buffer_.data(), samples + sampleCount - kFade,
                        kFade * sizeof(float));
        } else {
            size_t pad = kFade - sampleCount;
            std::memset(buffer_.data(), 0, pad * sizeof(float));
            std::memcpy(buffer_.data() + pad, samples,
                        sampleCount * sizeof(float));
        }
    }

    // Blend the saved fade-out tail into the start of new output (fade-in).
    // No-op if no fade is pending. RT-safe; consumes the pending flag.
    HOT_FUNCTION
    void applyCrossfade(float* output, size_t sampleCount) noexcept {
        if (!pending_) return;
        pending_ = false;
        constexpr size_t kFade = config::crossfade::kFadeSamples;
        size_t fadeSamples = sampleCount < kFade ? sampleCount : kFade;
        if (fadeSamples == 0) return;
        float fadeStep = 1.0f / static_cast<float>(fadeSamples);
        for (size_t i = 0; i < fadeSamples; ++i) {
            float fadeOut = 1.0f - static_cast<float>(i) * fadeStep;
            float fadeIn = static_cast<float>(i) * fadeStep;
            output[i] = buffer_[i] * fadeOut + output[i] * fadeIn;
        }
    }

    // Reset all state. Called from the RT path when a deferred clear is serviced.
    void reset() noexcept {
        pending_ = false;
        active_ = false;
    }

private:
    alignas(config::kSimdAlignment) std::array<float, config::crossfade::kFadeSamples> buffer_{};
    bool pending_{false};
    bool active_{false};
};

}  // namespace media
