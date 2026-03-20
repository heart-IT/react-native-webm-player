#include "JSIInstaller.h"
#include <jsi/jsi.h>
#include <ReactCommon/CallInvoker.h>
#include "playback/MediaPipelineModule.h"
#include "video/VideoPipelineModule.h"

bool installJSIInstaller(
    facebook::jsi::Runtime& rt,
    std::shared_ptr<facebook::react::CallInvoker> jsCallInvoker
) {
    if (!jsCallInvoker) {
        return false;
    }

    bool pipelineOk = mediamodule::MediaPipelineModule::install(rt, jsCallInvoker);
    bool videoPipelineOk = videomodule::VideoPipelineModule::install(rt, jsCallInvoker);

    // Wire A/V sync: pass video's sync coordinator to audio decode channel
    if (pipelineOk && videoPipelineOk) {
        uintptr_t rtId = mediamodule::MediaPipelineRegistry::getRuntimeId(rt);
        auto audioModule = mediamodule::MediaPipelineRegistry::instance().getModule(rtId);
        auto videoModule = videomodule::VideoPipelineRegistry::instance().getModule(rtId);
        if (audioModule && videoModule) {
            audioModule->setSyncCoordinator(&videoModule->syncCoordinator());
            audioModule->setVideoQueue(&videoModule->frameQueue());
        }
    }

    return pipelineOk && videoPipelineOk;
}

void uninstallJSIInstaller(facebook::jsi::Runtime& rt) {
    // Clear A/V sync coordinator before video module is destroyed
    {
        uintptr_t rtId = mediamodule::MediaPipelineRegistry::getRuntimeId(rt);
        auto audioModule = mediamodule::MediaPipelineRegistry::instance().getModule(rtId);
        if (audioModule) {
            audioModule->setVideoQueue(nullptr);
            audioModule->setSyncCoordinator(nullptr);
        }
    }

    videomodule::VideoPipelineModule::uninstall(rt);
    mediamodule::MediaPipelineModule::uninstall(rt);
}
