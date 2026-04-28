#include "VideoPipelineModule.h"
#include "MediaCodecDecoder.h"
#include "common/MediaLog.h"
#include "video/VideoSurfaceRegistry.h"
#include <android/native_window_jni.h>

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
        if (s) ANativeWindow_acquire(static_cast<ANativeWindow*>(s));
    });
    surfaceRegistry.setSurfaceReleaseFn([](media::VideoSurface s) {
        if (s) ANativeWindow_release(static_cast<ANativeWindow*>(s));
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

    // Platform decoder factory: creates MediaCodecDecoder with acquired surface.
    // Returns nullptr until a VideoView mounts; VideoDecodeThread retries with backoff.
    // keyFrameRequestFn routes through VideoFrameQueue so both decoder error
    // paths clear the queue and fire the JS keyFrameNeeded callback through one
    // canonical path (parity with iOS VP9Decoder).
    auto* fq = &frameQueue_;
    auto factory = [fq]() -> std::unique_ptr<media::VideoDecoder> {
        auto* surface = static_cast<ANativeWindow*>(
            media::VideoSurfaceRegistry::instance().acquireSurface());
        if (!surface) return nullptr;

        auto dec = std::make_unique<MediaCodecDecoder>();
        if (!dec->initialize(surface)) {
            // initialize() takes ownership and releases surface on failure
            MEDIA_LOG_E("VideoPipelineModule: failed to init decoder");
            return nullptr;
        }
        dec->setKeyFrameRequestFn([fq]() {
            fq->requestKeyFrame();
        });
        return dec;
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
