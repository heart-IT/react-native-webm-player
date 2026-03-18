#pragma once

#include <mutex>
#include <unordered_map>
#include <functional>
#include <utility>

namespace media {

// Platform-specific surface handle:
//   Android: ANativeWindow*
//   iOS: (__bridge void*) to AVSampleBufferDisplayLayer
using VideoSurface = void*;

using KeyFrameRequestFn = std::function<void(int participantId)>;
using SurfaceReleaseFn = std::function<void(VideoSurface)>;
using SurfaceAcquireFn = std::function<void(VideoSurface)>;

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

    // Returns surface with caller-owned reference (must be released via releaseFn).
    // Returns nullptr if participant not found. Holds lock during acquire.
    VideoSurface acquireSurface(int participantId) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = surfaces_.find(participantId);
        if (it == surfaces_.end() || !it->second) return nullptr;
        if (acquireFn_) acquireFn_(it->second);
        return it->second;
    }

    // Takes ownership of one ref on `surface`. Caller must not release after this call.
    void registerSurface(int participantId, VideoSurface surface) {
        KeyFrameRequestFn cb;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = surfaces_.find(participantId);
            if (it != surfaces_.end()) {
                if (it->second == surface) {
                    // Same pointer re-registered — release caller's extra ref
                    if (releaseFn_) releaseFn_(surface);
                    return;
                }
                if (releaseFn_) releaseFn_(it->second);
            }
            surfaces_[participantId] = surface;
            cb = keyFrameRequestFn_;
        }
        if (cb) {
            cb(participantId);
        }
    }

    void unregisterSurface(int participantId) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = surfaces_.find(participantId);
        if (it != surfaces_.end()) {
            if (releaseFn_) releaseFn_(it->second);
            surfaces_.erase(it);
        }
        decodedResolutions_.erase(participantId);
    }

    bool hasSurface(int participantId) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = surfaces_.find(participantId);
        return it != surfaces_.end() && it->second != nullptr;
    }

    void setKeyFrameRequestFn(KeyFrameRequestFn fn) {
        std::lock_guard<std::mutex> lk(mtx_);
        keyFrameRequestFn_ = std::move(fn);
    }

    void requestKeyFrame(int participantId) {
        KeyFrameRequestFn cb;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            cb = keyFrameRequestFn_;
        }
        if (cb) cb(participantId);
    }

    void setDecodedResolution(int participantId, int w, int h) {
        std::lock_guard<std::mutex> lk(mtx_);
        decodedResolutions_[participantId] = {w, h};
    }

    std::pair<int,int> getDecodedResolution(int participantId) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = decodedResolutions_.find(participantId);
        return it != decodedResolutions_.end() ? it->second : std::pair<int,int>{0, 0};
    }

    void clear() {
        std::lock_guard<std::mutex> lk(mtx_);
        if (releaseFn_) {
            for (auto& [_, s] : surfaces_) releaseFn_(s);
        }
        surfaces_.clear();
        decodedResolutions_.clear();
        keyFrameRequestFn_ = nullptr;
        releaseFn_ = nullptr;
        acquireFn_ = nullptr;
    }

private:
    VideoSurfaceRegistry() = default;
    std::mutex mtx_;
    std::unordered_map<int, VideoSurface> surfaces_;
    std::unordered_map<int, std::pair<int,int>> decodedResolutions_;
    KeyFrameRequestFn keyFrameRequestFn_;
    SurfaceReleaseFn releaseFn_;
    SurfaceAcquireFn acquireFn_;
};

}  // namespace media
