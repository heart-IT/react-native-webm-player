#pragma once

#include <jsi/jsi.h>
#include <ReactCommon/CallInvoker.h>
#include <atomic>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <array>
#include <thread>
#include "../../cpp/common/ModuleRegistry.h"
#include "../../cpp/video/VideoDecodeSlot.h"
#include "../../cpp/video/VideoConfig.h"
#include "../../cpp/video/VideoSyncController.h"
#include "../../cpp/video/FpsCounter.h"
#include "../../cpp/common/AVSyncCoordinator.h"

// Forward declare
namespace videomodule { class VP9Decoder; }

namespace videomodule {

class VideoPipelineModule;
using VideoPipelineRegistry = media::ModuleRegistry<VideoPipelineModule>;

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

    // Access slots for the VP9Decoder to pull frames
    media::VideoDecodeSlot* getSlot(int slotIdx) {
        if (slotIdx < 0 || slotIdx >= media::video_config::kMaxVideoSlots) return nullptr;
        return &slots_[slotIdx];
    }

    int slotForParticipant(int participantId) {
        std::lock_guard<std::mutex> lk(slotMapMtx_);
        auto it = participantToSlot_.find(participantId);
        return it != participantToSlot_.end() ? it->second : -1;
    }

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

    // Decode thread: drains all slots and feeds per-participant VP9Decoders
    void startDecodeThread();
    void stopDecodeThread();
    void decodeLoop();

    std::thread decodeThread_;
    std::atomic<bool> decodeRunning_{false};
    std::array<std::unique_ptr<VP9Decoder>, media::video_config::kMaxVideoSlots> decoders_;
    std::mutex decoderMtx_;

    std::array<media::VideoRenderClock, media::video_config::kMaxVideoSlots> renderClocks_;
    std::array<media::FpsCounter, media::video_config::kMaxVideoSlots> fpsCounters_;
    std::array<int64_t, media::video_config::kMaxVideoSlots> lastAnchorTimeUs_{};
    media::AVSyncCoordinator syncCoordinator_;

    std::shared_ptr<facebook::react::CallInvoker> callInvoker_;
    facebook::jsi::Runtime* runtime_{nullptr};

public:
    media::AVSyncCoordinator& syncCoordinator() noexcept { return syncCoordinator_; }
};

}  // namespace videomodule
