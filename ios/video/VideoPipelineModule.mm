#include "VideoPipelineModule.h"
#include "VP9Decoder.h"
#include "../../cpp/common/MediaLog.h"
#include "../../cpp/video/VideoSurfaceRegistry.h"
#import <AVFoundation/AVFoundation.h>

namespace videomodule {

namespace jsi = facebook::jsi;

bool VideoPipelineModule::install(jsi::Runtime& rt,
                                   std::shared_ptr<facebook::react::CallInvoker> callInvoker) {
    auto& registry = VideoPipelineRegistry::instance();
    uintptr_t rtId = VideoPipelineRegistry::getRuntimeId(rt);

    if (registry.getModule(rtId)) {
        MEDIA_LOG_W("VideoPipelineModule: already installed for this runtime");
        return true;
    }

    auto& surfaceRegistry = media::VideoSurfaceRegistry::instance();
    surfaceRegistry.setSurfaceAcquireFn([](media::VideoSurface s) {
        if (s) CFRetain(s);
    });
    surfaceRegistry.setSurfaceReleaseFn([](media::VideoSurface s) {
        if (s) CFRelease(s);
    });

    auto module = std::make_shared<VideoPipelineModule>();
    registry.registerModule(rtId, module);

    MEDIA_LOG_I("VideoPipelineModule: installed");
    return true;
}

void VideoPipelineModule::uninstall(jsi::Runtime& rt) {
    auto& registry = VideoPipelineRegistry::instance();
    uintptr_t rtId = VideoPipelineRegistry::getRuntimeId(rt);

    auto module = registry.getModule(rtId);
    if (!module) return;

    module->stop();

    media::VideoSurfaceRegistry::instance().clear();

    registry.unregisterModule(rtId);

    MEDIA_LOG_I("VideoPipelineModule: uninstalled");
}

bool VideoPipelineModule::start() {
    std::lock_guard<std::mutex> lk(lifecycleMtx_);
    if (running_.load(std::memory_order_acquire)) return true;
    running_.store(true, std::memory_order_release);
    frameQueue_.reset();

    // Platform decoder factory: creates VP9Decoder with acquired surface.
    // Returns nullptr until a VideoView mounts; VideoDecodeThread retries with backoff.
    // keyFrameRequestFn routes through VideoFrameQueue so both sync (submitFrame
    // ladder) and async (VTDecompressionSession callback) error paths clear the
    // queue and fire the JS keyFrameNeeded callback through one canonical path.
    auto* fq = &frameQueue_;
    auto factory = [fq]() -> std::unique_ptr<media::VideoDecoder> {
        auto surface = media::VideoSurfaceRegistry::instance().acquireSurface();
        if (!surface) return nullptr;

        auto d = std::make_unique<VP9Decoder>();
        d->setOutputLayer((__bridge AVSampleBufferDisplayLayer*)surface);
        CFRelease(surface);
        d->setKeyFrameRequestFn([fq]() {
            fq->requestKeyFrame();
        });
        return d;
    };

    if (!decodeThread_.start(&frameQueue_, std::move(factory))) {
        MEDIA_LOG_E("VideoPipelineModule: failed to start decode thread");
        running_.store(false, std::memory_order_release);
        return false;
    }
    MEDIA_LOG_I("VideoPipelineModule: started");
    return true;
}

bool VideoPipelineModule::stop() {
    std::lock_guard<std::mutex> lk(lifecycleMtx_);
    if (!running_.load(std::memory_order_acquire)) return true;
    running_.store(false, std::memory_order_release);
    decodeThread_.stop();
    frameQueue_.reset();
    MEDIA_LOG_I("VideoPipelineModule: stopped");
    return true;
}

}  // namespace videomodule
