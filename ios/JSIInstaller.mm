#include "JSIInstaller.h"
#include <jsi/jsi.h>
#include <ReactCommon/CallInvoker.h>
#include "playback/MediaPipelineModule.h"
#include "video/VideoPipelineModule.h"
#include "playback_v2/MediaPipelineModuleV2.h"

bool installJSIInstaller(
    facebook::jsi::Runtime& rt,
    std::shared_ptr<facebook::react::CallInvoker> jsCallInvoker
) {
    if (!jsCallInvoker) {
        return false;
    }

    bool pipelineOk = mediamodule::MediaPipelineModule::install(rt, jsCallInvoker);
    bool videoPipelineOk = videomodule::VideoPipelineModule::install(rt, jsCallInvoker);

    // Wire A/V sync: pass video's sync coordinator to audio decode channel,
    // then start the video decode thread (idles on backoff until a surface is acquired).
    if (pipelineOk && videoPipelineOk) {
        uintptr_t rtId = mediamodule::MediaPipelineRegistry::getRuntimeId(rt);
        auto audioModule = mediamodule::MediaPipelineRegistry::instance().getModule(rtId);
        auto videoModule = videomodule::VideoPipelineRegistry::instance().getModule(rtId);
        if (audioModule && videoModule) {
            audioModule->setSyncCoordinator(&videoModule->syncCoordinator());
            audioModule->setVideoQueue(&videoModule->frameQueue());
            videoModule->start();
        }
    }

    // v2 engine installed alongside v1 during migration. Reachable as
    // global __MediaPipelineV2. v1 globals stay live so the existing app keeps
    // working until Phase 6 deletion.
    mediamodule_v2::installV2JSI(rt, jsCallInvoker);

    return pipelineOk && videoPipelineOk;
}

void uninstallJSIInstaller(facebook::jsi::Runtime& rt) {
    mediamodule_v2::uninstallV2JSI(rt);

    // Audio module dies FIRST so the decode thread (and its HealthWatchdog) is
    // joined before VideoFrameQueue and AVSyncCoordinator are destroyed.
    // The watchdog's readMetrics lambda holds raw videoQueue / syncCoordinator
    // pointers; the audio render path holds an atomic AVSyncCoordinator*.
    // Reversing this order would leave a UAF window between video module
    // destruction and audio decode-thread join.
    mediamodule::MediaPipelineModule::uninstall(rt);
    videomodule::VideoPipelineModule::uninstall(rt);
}
