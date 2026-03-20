#pragma once

#include <jsi/jsi.h>
#include <ReactCommon/CallInvoker.h>
#include <atomic>
#include <mutex>
#include <memory>
#include "MediaSession.h"
#include "common/AudioRouteTypes.h"
#include "common/ModuleRegistry.h"
#include "demux/WebmDemuxer.h"
#include "common/ClipBuffer.h"
#include "video/VideoFrameQueue.h"
#include "video/VideoSurfaceRegistry.h"

namespace mediamodule {

// LOCK ORDERING: When acquiring multiple locks, always acquire in this order:
//   1. MediaPipelineRegistry::lock_ (outermost)
//   2. MediaPipelineModule::lifecycleMtx_ (innermost)
// Never hold lifecycleMtx_ while calling registry methods.

class MediaPipelineModule;
using MediaPipelineRegistry = media::ModuleRegistry<MediaPipelineModule>;

class MediaPipelineModule : public facebook::jsi::HostObject,
                            public std::enable_shared_from_this<MediaPipelineModule> {
public:
    MediaPipelineModule() = default;
    ~MediaPipelineModule() override = default;

    void setSyncCoordinator(media::AVSyncCoordinator* coordinator) noexcept {
        syncCoordinator_ = coordinator;
        session_.setSyncCoordinator(coordinator);
    }

    void setVideoQueue(media::VideoFrameQueue* queue) noexcept {
        videoQueue_ = queue;
        if (queue) {
            media::VideoSurfaceRegistry::instance().setSurfaceLostFn([queue]() {
                queue->metrics().surfaceLostCount.fetch_add(1, std::memory_order_relaxed);
                queue->requestKeyFrame();
            });
        }
    }

    static bool install(facebook::jsi::Runtime& rt,
                        std::shared_ptr<facebook::react::CallInvoker> callInvoker = nullptr);
    static void uninstall(facebook::jsi::Runtime& rt);
    static std::shared_ptr<MediaPipelineModule> getForRuntime(facebook::jsi::Runtime& rt);

    facebook::jsi::Value get(facebook::jsi::Runtime& rt, const facebook::jsi::PropNameID& name) override;
    std::vector<facebook::jsi::PropNameID> getPropertyNames(facebook::jsi::Runtime& rt) override;

private:
    bool ensureRunning();

    facebook::jsi::Value feedData(facebook::jsi::Runtime& rt,
                                    const facebook::jsi::Value& buffer);
    facebook::jsi::Value setMuted(facebook::jsi::Runtime& rt, bool muted);
    facebook::jsi::Value setGain(facebook::jsi::Runtime& rt, float gain);
    facebook::jsi::Value getMetrics(facebook::jsi::Runtime& rt);
    facebook::jsi::Value start(facebook::jsi::Runtime& rt);
    facebook::jsi::Value stop(facebook::jsi::Runtime& rt);
    facebook::jsi::Value isRunning(facebook::jsi::Runtime& rt);
    facebook::jsi::Value setAudioRouteCallback(facebook::jsi::Runtime& rt, const facebook::jsi::Value& callback);
    facebook::jsi::Value setHealthCallback(facebook::jsi::Runtime& rt, const facebook::jsi::Value& callback);
    facebook::jsi::Value getTrackInfo(facebook::jsi::Runtime& rt);
    facebook::jsi::Value resetStream(facebook::jsi::Runtime& rt);
    facebook::jsi::Value requestKeyFrame(facebook::jsi::Runtime& rt);
    facebook::jsi::Value warmUp(facebook::jsi::Runtime& rt);
    facebook::jsi::Value setKeyFrameNeededCallback(facebook::jsi::Runtime& rt, const facebook::jsi::Value& callback);
    facebook::jsi::Value pause(facebook::jsi::Runtime& rt);
    facebook::jsi::Value resume(facebook::jsi::Runtime& rt);
    facebook::jsi::Value isPaused(facebook::jsi::Runtime& rt);
    facebook::jsi::Value getCurrentTimeUs(facebook::jsi::Runtime& rt);
    facebook::jsi::Value getPlaybackState(facebook::jsi::Runtime& rt);
    facebook::jsi::Value setBufferTarget(facebook::jsi::Runtime& rt, double audioMs, double videoMs);
    facebook::jsi::Value setAudioFocusCallback(facebook::jsi::Runtime& rt, const facebook::jsi::Value& callback);

    void setupAudioRouteCallback(facebook::jsi::Runtime& rt);
    void setupHealthWatchdog();
    void setupKeyFrameNeededCallback();

    std::mutex lifecycleMtx_;
    media::MediaSession session_;
    media::demux::WebmDemuxer demuxer_;
    media::ClipBuffer clipBuffer_;
    media::VideoFrameQueue* videoQueue_{nullptr};
    media::AVSyncCoordinator* syncCoordinator_{nullptr};

    // Callback support
    std::shared_ptr<facebook::react::CallInvoker> callInvoker_;
    std::shared_ptr<facebook::jsi::Function> routeCallback_;
    std::shared_ptr<facebook::jsi::Function> healthCallback_;
    std::shared_ptr<facebook::jsi::Function> keyFrameNeededCallback_;
    std::shared_ptr<facebook::jsi::Function> audioFocusCallback_;
    bool autoPausedByFocus_{false};
    facebook::jsi::Runtime* runtime_{nullptr};
};

}  // namespace mediamodule
