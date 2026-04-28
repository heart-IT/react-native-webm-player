// Packet-loss concealment crossfade — RT-thread only.
//
// When the decoder switches from PLC (synthetic) frames back to real audio,
// a 5ms equal-power crossfade smooths the transition: the tail of the last
// concealed output fades out while the head of the next real frame fades in.
//
// Lifecycle (all on the audio callback thread):
//   1. saveTail()             — at the PLC→real transition, capture the last
//                               kFadeSamples of just-output concealed audio.
//   2. applyCrossfade()       — on the next output buffer, blend saved tail
//                               with the start of the real audio. Consumes
//                               the pending flag.
//   3. setLastFrameConcealed() — track the concealment state of the last
//                               consumed frame so the caller knows when a
//                               PLC→real transition occurs.
#pragma once

#include "common/CompilerHints.h"
#include "common/MediaConfig.h"

#include <array>
#include <cstring>

namespace media {

class PLCConcealer {
public:
    // Save the last kFadeSamples of just-output concealed audio. Marks
    // a crossfade pending for the next applyCrossfade() call. RT-safe.
    HOT_FUNCTION
    void saveTail(const float* buffer, size_t sampleCount) noexcept {
        constexpr size_t kFade = config::crossfade::kFadeSamples;
        if (sampleCount >= kFade) {
            std::memcpy(buffer_.data(), buffer + sampleCount - kFade,
                        kFade * sizeof(float));
        } else if (sampleCount > 0) {
            size_t pad = kFade - sampleCount;
            std::memset(buffer_.data(), 0, pad * sizeof(float));
            std::memcpy(buffer_.data() + pad, buffer,
                        sampleCount * sizeof(float));
        } else {
            return;
        }
        pending_ = true;
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

    [[nodiscard]] bool wasLastFrameConcealed() const noexcept { return lastConcealed_; }
    void setLastFrameConcealed(bool b) noexcept { lastConcealed_ = b; }

    // Reset all state. Called from the RT path when a deferred clear is serviced.
    void reset() noexcept {
        pending_ = false;
        lastConcealed_ = false;
    }

private:
    alignas(config::kSimdAlignment) std::array<float, config::crossfade::kFadeSamples> buffer_{};
    bool pending_{false};
    bool lastConcealed_{false};
};

}  // namespace media
