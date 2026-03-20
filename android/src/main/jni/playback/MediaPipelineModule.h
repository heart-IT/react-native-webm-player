#pragma once

#include <jsi/jsi.h>
#include <ReactCommon/CallInvoker.h>
#include <memory>
#include "MediaSession.h"
#include "AudioSessionManager.h"
#include "common/AudioRouteTypes.h"
#include "common/ModuleRegistry.h"
#include "common/PipelineOrchestrator.h"

namespace mediamodule {

class MediaPipelineModule;
using MediaPipelineRegistry = media::ModuleRegistry<MediaPipelineModule>;

class MediaPipelineModule : public facebook::jsi::HostObject,
                            public std::enable_shared_from_this<MediaPipelineModule> {
public:
    MediaPipelineModule() = default;
    ~MediaPipelineModule() override = default;

    void setSyncCoordinator(media::AVSyncCoordinator* c) noexcept { orchestrator_.setSyncCoordinator(c); }
    void setVideoQueue(media::VideoFrameQueue* q) noexcept { orchestrator_.setVideoQueue(q); }

    static bool install(facebook::jsi::Runtime& rt,
                        std::shared_ptr<facebook::react::CallInvoker> callInvoker = nullptr);
    static void uninstall(facebook::jsi::Runtime& rt);
    static std::shared_ptr<MediaPipelineModule> getForRuntime(facebook::jsi::Runtime& rt);

    facebook::jsi::Value get(facebook::jsi::Runtime& rt, const facebook::jsi::PropNameID& name) override {
        return orchestrator_.get(rt, name);
    }

    std::vector<facebook::jsi::PropNameID> getPropertyNames(facebook::jsi::Runtime& rt) override {
        return orchestrator_.getPropertyNames(rt);
    }

    PipelineOrchestrator& orchestrator() noexcept { return orchestrator_; }

private:
    PipelineOrchestrator orchestrator_;
};

}  // namespace mediamodule
