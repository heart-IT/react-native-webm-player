#pragma once

#include <jsi/jsi.h>
#include <ReactCommon/CallInvoker.h>
#include <atomic>
#include <mutex>
#include <memory>
#include "MediaSession.h"
#include "../../cpp/common/AudioRouteTypes.h"
#include "../../cpp/common/ModuleRegistry.h"

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
        session_.setSyncCoordinator(coordinator);
    }

    static bool install(facebook::jsi::Runtime& rt,
                        std::shared_ptr<facebook::react::CallInvoker> callInvoker = nullptr);
    static void uninstall(facebook::jsi::Runtime& rt);
    static std::shared_ptr<MediaPipelineModule> getForRuntime(facebook::jsi::Runtime& rt);

    facebook::jsi::Value get(facebook::jsi::Runtime& rt, const facebook::jsi::PropNameID& name) override;
    std::vector<facebook::jsi::PropNameID> getPropertyNames(facebook::jsi::Runtime& rt) override;

private:
    bool ensureRunning();

    facebook::jsi::Value addParticipant(facebook::jsi::Runtime& rt);
    facebook::jsi::Value removeParticipant(facebook::jsi::Runtime& rt, int participantId);
    facebook::jsi::Value pushAudioFrame(facebook::jsi::Runtime& rt, int participantId,
                                         const facebook::jsi::Value& buffer,
                                         int64_t ptsUs, int64_t durationUs);
    facebook::jsi::Value setMuted(facebook::jsi::Runtime& rt, int participantId, bool muted);
    facebook::jsi::Value setGain(facebook::jsi::Runtime& rt, int participantId, float gain);
    facebook::jsi::Value getMetrics(facebook::jsi::Runtime& rt);
    facebook::jsi::Value warmUp(facebook::jsi::Runtime& rt);
    facebook::jsi::Value start(facebook::jsi::Runtime& rt);
    facebook::jsi::Value stop(facebook::jsi::Runtime& rt);
    facebook::jsi::Value isRunning(facebook::jsi::Runtime& rt);
    facebook::jsi::Value getLimits(facebook::jsi::Runtime& rt);
    facebook::jsi::Value resetWatchdog(facebook::jsi::Runtime& rt);
    facebook::jsi::Value setEvictionCallback(facebook::jsi::Runtime& rt, const facebook::jsi::Value& callback);
    facebook::jsi::Value setAudioRouteCallback(facebook::jsi::Runtime& rt, const facebook::jsi::Value& callback);

    void setupEvictionCallback(facebook::jsi::Runtime& rt);
    void setupAudioRouteCallback(facebook::jsi::Runtime& rt);

    std::mutex lifecycleMtx_;
    media::MediaSession session_;

    // Callback support
    std::shared_ptr<facebook::react::CallInvoker> callInvoker_;
    std::shared_ptr<facebook::jsi::Function> evictionCallback_;
    std::shared_ptr<facebook::jsi::Function> routeCallback_;
    facebook::jsi::Runtime* runtime_{nullptr};
};

}  // namespace mediamodule
