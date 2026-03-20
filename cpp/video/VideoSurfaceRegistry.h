#pragma once

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
        KeyFrameRequestFn cb;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            cb = keyFrameRequestFn_;
        }
        if (cb) cb();
    }

    void setDecodedResolution(int w, int h) {
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
};

}  // namespace media
