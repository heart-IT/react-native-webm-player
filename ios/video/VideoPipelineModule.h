#pragma once

#include <jsi/jsi.h>
#include <ReactCommon/CallInvoker.h>
#include <atomic>
#include <mutex>
#include <memory>
#include "../../cpp/common/ModuleRegistry.h"
#include "../../cpp/video/VideoFrameQueue.h"
#include "../../cpp/video/VideoConfig.h"
#include "../../cpp/video/VideoDecodeThread.h"

namespace videomodule {

class VideoPipelineModule;
using VideoPipelineRegistry = media::ModuleRegistry<VideoPipelineModule>;

// Single-stream video pipeline for broadcast WebM playback.
// Not exposed to JS — the audio MediaPipelineModule is the JSI surface; this module
// is registered for A/V-sync wiring and driven directly from JSIInstaller.
class VideoPipelineModule : public std::enable_shared_from_this<VideoPipelineModule> {
public:
    VideoPipelineModule() = default;
    ~VideoPipelineModule() = default;

    static bool install(facebook::jsi::Runtime& rt,
                        std::shared_ptr<facebook::react::CallInvoker> callInvoker = nullptr);
    static void uninstall(facebook::jsi::Runtime& rt);

    // Starts the video decode thread; decoder factory will retry until a render surface is acquired.
    bool start();
    bool stop();
    bool isRunning() const noexcept { return running_.load(std::memory_order_acquire); }

    media::AVSyncCoordinator& syncCoordinator() noexcept { return decodeThread_.syncCoordinator(); }
    media::VideoFrameQueue& frameQueue() noexcept { return frameQueue_; }

private:
    std::mutex lifecycleMtx_;
    std::atomic<bool> running_{false};

    media::VideoFrameQueue frameQueue_;
    media::VideoDecodeThread decodeThread_;
};

}  // namespace videomodule
