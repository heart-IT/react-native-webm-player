#include "JSIInstaller.h"
#include <jsi/jsi.h>
#include <ReactCommon/CallInvoker.h>
#include "mediapipeline_install.h"
#include "videopipeline_install.h"
#include "playback/MediaPipelineModule.h"
#include "video/VideoPipelineModule.h"

bool installJSIInstaller(
    facebook::jsi::Runtime& rt,
    std::shared_ptr<facebook::react::CallInvoker> jsCallInvoker
) {
    if (!jsCallInvoker) {
        return false;
    }

    bool pipelineOk = installMediaPipeline(rt, jsCallInvoker);
    bool videoPipelineOk = installVideoPipeline(rt, jsCallInvoker);

    // Wire A/V sync: pass video's sync coordinator to audio decode slots
    if (pipelineOk && videoPipelineOk) {
        uintptr_t rtId = mediamodule::MediaPipelineRegistry::getRuntimeId(rt);
        auto audioModule = mediamodule::MediaPipelineRegistry::instance().getModule(rtId);
        auto videoModule = videomodule::VideoPipelineRegistry::instance().getModule(rtId);
        if (audioModule && videoModule) {
            audioModule->setSyncCoordinator(&videoModule->syncCoordinator());
        }
    }

    return pipelineOk && videoPipelineOk;
}

void uninstallJSIInstaller(facebook::jsi::Runtime& rt) {
    // Clear A/V sync coordinator before video module is destroyed
    // to prevent dangling pointer in audio decode slots
    {
        uintptr_t rtId = mediamodule::MediaPipelineRegistry::getRuntimeId(rt);
        auto audioModule = mediamodule::MediaPipelineRegistry::instance().getModule(rtId);
        if (audioModule) {
            audioModule->setSyncCoordinator(nullptr);
        }
    }

    uninstallVideoPipeline(rt);
    uninstallMediaPipeline(rt);
}
