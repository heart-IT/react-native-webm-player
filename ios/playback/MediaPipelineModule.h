// iOS JSI module: exposes MediaPipeline API to JavaScript via JSI.
// Handles feedData(), demuxing, audio pipeline, health watchdog, and clip buffer.
// Video is handled by the separate VideoPipelineModule.
//
// JSI exposure: install() builds a plain jsi::Object with each method pre-bound
// as a Function property (Margelo's "Make JSI run faster" pattern, ~5× faster
// than HostObject + dispatch). The module itself is held in the registry as a
// shared_ptr to anchor the orchestrator and its captured weak_ptr lifetime
// guards in the bound Functions.
#pragma once

#include <jsi/jsi.h>
#include <ReactCommon/CallInvoker.h>
#include <memory>
#include "MediaSession.h"
#include "AudioSessionManager.h"
#include "../../cpp/common/AudioRouteTypes.h"
#include "../../cpp/common/ModuleRegistry.h"
#include "../../cpp/common/PipelineOrchestrator.h"

namespace mediamodule {

class MediaPipelineModule;
using MediaPipelineRegistry = media::ModuleRegistry<MediaPipelineModule>;

class MediaPipelineModule {
public:
    MediaPipelineModule() = default;
    ~MediaPipelineModule() = default;

    MediaPipelineModule(const MediaPipelineModule&) = delete;
    MediaPipelineModule& operator=(const MediaPipelineModule&) = delete;

    void setSyncCoordinator(media::AVSyncCoordinator* c) noexcept { orchestrator_.setSyncCoordinator(c); }
    void setVideoQueue(media::VideoFrameQueue* q) noexcept { orchestrator_.setVideoQueue(q); }

    static bool install(facebook::jsi::Runtime& rt,
                        std::shared_ptr<facebook::react::CallInvoker> callInvoker = nullptr);
    static void uninstall(facebook::jsi::Runtime& rt);
    static std::shared_ptr<MediaPipelineModule> getForRuntime(facebook::jsi::Runtime& rt);

    PipelineOrchestrator& orchestrator() noexcept { return orchestrator_; }

private:
    PipelineOrchestrator orchestrator_;
};

}  // namespace mediamodule
