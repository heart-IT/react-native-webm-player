#include "MediaPipelineModule.h"
#include "../../cpp/common/MediaLog.h"
#include "../../cpp/common/ThreadAffinity.h"
#include "../../cpp/video/VideoSurfaceRegistry.h"
#include "../../cpp/common/ClipIndex.h"
#import <Foundation/Foundation.h>

namespace mediamodule {

namespace jsi = facebook::jsi;

bool MediaPipelineModule::install(jsi::Runtime& rt,
                                   std::shared_ptr<facebook::react::CallInvoker> callInvoker) {
    auto& registry = MediaPipelineRegistry::instance();
    uintptr_t rtId = MediaPipelineRegistry::getRuntimeId(rt);

    if (registry.getModule(rtId)) {
        MEDIA_LOG_W("MediaPipelineModule: already installed for this runtime");
        return true;
    }

    media::thread_affinity::initializeCoreConfig();

    auto module = std::make_shared<MediaPipelineModule>();
    module->orchestrator().init(callInvoker, &rt, module);
    registry.registerModule(rtId, module);

    @autoreleasepool {
        NSString* tempDir = NSTemporaryDirectory();
        if (tempDir) {
            media::ClipIndex::setTempDir([tempDir UTF8String]);
        }
    }

    // Pre-bind all methods as Function properties on a plain jsi::Object.
    // Subsequent JS accesses (e.g. __MediaPipeline.feedData(buf)) hit Hermes'
    // native property hashtable and return the cached Function — no per-call
    // prop.utf8 allocation, no Function recreation. Margelo's "Make JSI run
    // faster" benchmarks this pattern at ~5× the speed of HostObject + dispatch.
    auto api = module->orchestrator().createApiObject(rt);
    rt.global().setProperty(rt, "__MediaPipeline", std::move(api));

    MEDIA_LOG_I("MediaPipelineModule: installed");
    return true;
}

void MediaPipelineModule::uninstall(jsi::Runtime& rt) {
    auto& registry = MediaPipelineRegistry::instance();
    uintptr_t rtId = MediaPipelineRegistry::getRuntimeId(rt);

    auto module = registry.getModule(rtId);
    if (!module) return;

    module->orchestrator().teardown();

    rt.global().setProperty(rt, "__MediaPipeline", jsi::Value::undefined());
    registry.unregisterModule(rtId);

    MEDIA_LOG_I("MediaPipelineModule: uninstalled");
}

std::shared_ptr<MediaPipelineModule> MediaPipelineModule::getForRuntime(jsi::Runtime& rt) {
    return MediaPipelineRegistry::instance().getModule(MediaPipelineRegistry::getRuntimeId(rt));
}

}  // namespace mediamodule
