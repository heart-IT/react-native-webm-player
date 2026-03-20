#pragma once

#include <jsi/jsi.h>
#include <ReactCommon/CallInvoker.h>
#include <atomic>
#include <mutex>
#include <memory>
#include <thread>
#include "../../cpp/common/ModuleRegistry.h"
#include "../../cpp/video/VideoFrameQueue.h"
#include "../../cpp/video/VideoConfig.h"
#include "../../cpp/video/VideoSyncController.h"
#include "../../cpp/video/FpsCounter.h"
#include "../../cpp/common/AVSyncCoordinator.h"

// Forward declare
namespace videomodule { class VP9Decoder; }

namespace videomodule {

class VideoPipelineModule;
using VideoPipelineRegistry = media::ModuleRegistry<VideoPipelineModule>;

// Single-stream video pipeline for broadcast WebM playback.
class VideoPipelineModule : public facebook::jsi::HostObject,
                            public std::enable_shared_from_this<VideoPipelineModule> {
public:
    VideoPipelineModule() = default;
    ~VideoPipelineModule() override;

    static bool install(facebook::jsi::Runtime& rt,
                        std::shared_ptr<facebook::react::CallInvoker> callInvoker = nullptr);
    static void uninstall(facebook::jsi::Runtime& rt);

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

    // Decode thread
    void startDecodeThread();
    void stopDecodeThread();
    void decodeLoop();

    std::thread decodeThread_;
    std::atomic<bool> decodeRunning_{false};
    std::unique_ptr<VP9Decoder> decoder_;
    std::mutex decoderMtx_;

    media::VideoRenderClock renderClock_;
    media::FpsCounter fpsCounter_;
    int64_t lastAnchorTimeUs_ = 0;
    media::AVSyncCoordinator syncCoordinator_;

    std::shared_ptr<facebook::react::CallInvoker> callInvoker_;
    facebook::jsi::Runtime* runtime_{nullptr};

public:
    media::AVSyncCoordinator& syncCoordinator() noexcept { return syncCoordinator_; }
    media::VideoFrameQueue& frameQueue() noexcept { return frameQueue_; }
};

}  // namespace videomodule
