#pragma once

#include <jsi/jsi.h>
#include <ReactCommon/CallInvoker.h>
#include <atomic>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <array>
#include "common/ModuleRegistry.h"
#include "common/AVSyncCoordinator.h"
#include "video/VideoDecodeSlot.h"
#include "video/VideoConfig.h"
#include "VideoDecodeThread.h"

namespace videomodule {

class VideoPipelineModule;
using VideoPipelineRegistry = media::ModuleRegistry<VideoPipelineModule>;

class VideoPipelineModule : public facebook::jsi::HostObject,
                            public std::enable_shared_from_this<VideoPipelineModule> {
public:
    VideoPipelineModule() = default;
    ~VideoPipelineModule() override = default;

    static bool install(facebook::jsi::Runtime& rt,
                        std::shared_ptr<facebook::react::CallInvoker> callInvoker = nullptr);
    static void uninstall(facebook::jsi::Runtime& rt);

    media::AVSyncCoordinator& syncCoordinator() noexcept { return decodeThread_.syncCoordinator(); }

    facebook::jsi::Value get(facebook::jsi::Runtime& rt, const facebook::jsi::PropNameID& name) override;
    std::vector<facebook::jsi::PropNameID> getPropertyNames(facebook::jsi::Runtime& rt) override;

private:
    facebook::jsi::Value start(facebook::jsi::Runtime& rt);
    facebook::jsi::Value stop(facebook::jsi::Runtime& rt);
    facebook::jsi::Value isRunning(facebook::jsi::Runtime& rt);
    facebook::jsi::Value pushVideoFrame(facebook::jsi::Runtime& rt, int participantId,
                                         const facebook::jsi::Value& buffer,
                                         int64_t ptsUs, bool isKeyFrame);
    facebook::jsi::Value removeParticipant(facebook::jsi::Runtime& rt, int participantId);
    facebook::jsi::Value requestKeyFrame(facebook::jsi::Runtime& rt, int participantId);
    facebook::jsi::Value getMetrics(facebook::jsi::Runtime& rt);

    int getOrCreateSlot(int participantId);
    void releaseSlot(int participantId);

    std::mutex lifecycleMtx_;
    std::atomic<bool> running_{false};

    std::array<media::VideoDecodeSlot, media::video_config::kMaxVideoSlots> slots_;
    std::mutex slotMapMtx_;
    std::unordered_map<int, int> participantToSlot_;

    VideoDecodeThread decodeThread_;

    std::shared_ptr<facebook::react::CallInvoker> callInvoker_;
    facebook::jsi::Runtime* runtime_{nullptr};
};

}  // namespace videomodule
