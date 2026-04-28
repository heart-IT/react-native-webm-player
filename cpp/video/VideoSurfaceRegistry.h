#pragma once

#include <atomic>
#include <mutex>
#include <functional>
#include <utility>

namespace media {

// Platform-specific surface handle:
//   Android: ANativeWindow*
//   iOS: (__bridge void*) to AVSampleBufferDisplayLayer
using VideoSurface = void*;

using KeyFrameRequestFn = std::function<void()>;
using SurfaceLostFn = std::function<void()>;
using SurfaceReleaseFn = std::function<void(VideoSurface)>;
using SurfaceAcquireFn = std::function<void(VideoSurface)>;

// Single-stream surface registry for broadcast playback.
// Thread safety: all methods are mutex-protected.
class VideoSurfaceRegistry {
public:
    static VideoSurfaceRegistry& instance() noexcept {
        static VideoSurfaceRegistry registry;
        return registry;
    }

    void setSurfaceReleaseFn(SurfaceReleaseFn fn) {
        std::lock_guard<std::mutex> lk(mtx_);
        releaseFn_ = std::move(fn);
    }

    void setSurfaceAcquireFn(SurfaceAcquireFn fn) {
        std::lock_guard<std::mutex> lk(mtx_);
        acquireFn_ = std::move(fn);
    }

    // Returns surface with caller-owned reference (must be released).
    // Returns nullptr if no surface registered.
    VideoSurface acquireSurface() {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!surface_) return nullptr;
        if (acquireFn_) acquireFn_(surface_);
        return surface_;
    }

    bool hasSurface() {
        std::lock_guard<std::mutex> lk(mtx_);
        return surface_ != nullptr;
    }

    // Takes ownership of one ref on `surface`. Caller must not release after this call.
    void registerSurface(VideoSurface surface) {
        KeyFrameRequestFn cb;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (surface_ == surface) {
                if (releaseFn_) releaseFn_(surface);
                return;
            }
            if (surface_ && releaseFn_) releaseFn_(surface_);
            surface_ = surface;
            cb = keyFrameRequestFn_;
        }
        if (cb) cb();
    }

    void unregisterSurface() {
        SurfaceLostFn lostCb;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (surface_) {
                if (releaseFn_) releaseFn_(surface_);
                surface_ = nullptr;
                lostCb = surfaceLostFn_;
            }
            decodedWidth_ = 0;
            decodedHeight_ = 0;
        }
        if (lostCb) lostCb();
    }

    void setSurfaceLostFn(SurfaceLostFn fn) {
        std::lock_guard<std::mutex> lk(mtx_);
        surfaceLostFn_ = std::move(fn);
    }

    void setKeyFrameRequestFn(KeyFrameRequestFn fn) {
        std::lock_guard<std::mutex> lk(mtx_);
        keyFrameRequestFn_ = std::move(fn);
    }

    void requestKeyFrame() {
        // Dedup: only fire JS on the false→true transition. Without this,
        // a wedged feedData ring (or sustained demux error) re-fires the
        // JS keyframe callback on every reject — see production-audit
        // finding I1 / P3-1. Reset latch when the next decoded frame
        // arrives (setDecodedResolution).
        bool wasAwaiting = keyFrameAwaiting_.exchange(true, std::memory_order_acq_rel);
        if (wasAwaiting) return;
        KeyFrameRequestFn cb;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            cb = keyFrameRequestFn_;
        }
        if (cb) cb();
    }

    void setDecodedResolution(int w, int h) {
        // First decoded frame after a keyframe-needed window arrives — clear
        // the latch so a subsequent surface-loss / wedge can re-arm the
        // request.
        keyFrameAwaiting_.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lk(mtx_);
        decodedWidth_ = w;
        decodedHeight_ = h;
    }

    std::pair<int,int> getDecodedResolution() {
        std::lock_guard<std::mutex> lk(mtx_);
        return {decodedWidth_, decodedHeight_};
    }

    void clear() {
        std::lock_guard<std::mutex> lk(mtx_);
        if (surface_ && releaseFn_) releaseFn_(surface_);
        surface_ = nullptr;
        decodedWidth_ = 0;
        decodedHeight_ = 0;
        keyFrameRequestFn_ = nullptr;
        surfaceLostFn_ = nullptr;
        releaseFn_ = nullptr;
        acquireFn_ = nullptr;
    }

private:
    VideoSurfaceRegistry() = default;
    std::mutex mtx_;
    VideoSurface surface_ = nullptr;
    int decodedWidth_ = 0;
    int decodedHeight_ = 0;
    KeyFrameRequestFn keyFrameRequestFn_;
    SurfaceLostFn surfaceLostFn_;
    SurfaceReleaseFn releaseFn_;
    SurfaceAcquireFn acquireFn_;
    // Atomic latch — set on requestKeyFrame, cleared on next setDecodedResolution.
    // Outside mtx_ so the dedup gate doesn't pay the lock on every reject path.
    std::atomic<bool> keyFrameAwaiting_{false};
};

}  // namespace media
