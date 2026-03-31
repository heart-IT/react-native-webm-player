#pragma once

#include <jsi/jsi.h>
#include <ReactCommon/CallInvoker.h>
#include <atomic>
#include <mutex>
#include <memory>
#include "common/ModuleRegistry.h"
#include "common/AVSyncCoordinator.h"
#include "video/VideoFrameQueue.h"
#include "video/VideoConfig.h"
#include "video/VideoDecodeThread.h"

namespace videomodule {

class VideoPipelineModule;
using VideoPipelineRegistry = media::ModuleRegistry<VideoPipelineModule>;

// Single-stream video pipeline for broadcast WebM playback.
class VideoPipelineModule : public facebook::jsi::HostObject,
                            public std::enable_shared_from_this<VideoPipelineModule> {
public:
    VideoPipelineModule() = default;
    ~VideoPipelineModule() override = default;

    static bool install(facebook::jsi::Runtime& rt,
                        std::shared_ptr<facebook::react::CallInvoker> callInvoker = nullptr);
    static void uninstall(facebook::jsi::Runtime& rt);

    media::AVSyncCoordinator& syncCoordinator() noexcept { return decodeThread_.syncCoordinator(); }
    media::VideoFrameQueue& frameQueue() noexcept { return frameQueue_; }

    facebook::jsi::Value get(facebook::jsi::Runtime& rt, const facebook::jsi::PropNameID& name) override;
    std::vector<facebook::jsi::PropNameID> getPropertyNames(facebook::jsi::Runtime& rt) override;

private:
    facebook::jsi::Value start(facebook::jsi::Runtime& rt);
    facebook::jsi::Value stop(facebook::jsi::Runtime& rt);
    facebook::jsi::Value isRunning(facebook::jsi::Runtime& rt);
    facebook::jsi::Value getMetrics(facebook::jsi::Runtime& rt);

    std::mutex lifecycleMtx_;
    std::atomic<bool> running_{false};

    media::VideoFrameQueue frameQueue_;
    media::VideoDecodeThread decodeThread_;
};

}  // namespace videomodule
