#include <jsi/jsi.h>
#include <ReactCommon/CallInvoker.h>
#include <android/native_window.h>
#include "VideoPipelineModule.h"
#include "video/VideoSurfaceRegistry.h"

extern "C" bool installVideoPipeline(facebook::jsi::Runtime& rt,
                                      std::shared_ptr<facebook::react::CallInvoker> callInvoker) {
    media::VideoSurfaceRegistry::instance().setSurfaceReleaseFn(
        [](media::VideoSurface s) { ANativeWindow_release(static_cast<ANativeWindow*>(s)); });
    media::VideoSurfaceRegistry::instance().setSurfaceAcquireFn(
        [](media::VideoSurface s) { ANativeWindow_acquire(static_cast<ANativeWindow*>(s)); });
    return videomodule::VideoPipelineModule::install(rt, callInvoker);
}

extern "C" void uninstallVideoPipeline(facebook::jsi::Runtime& rt) {
    videomodule::VideoPipelineModule::uninstall(rt);
}
