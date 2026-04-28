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

    return pipelineOk && videoPipelineOk;
}

void uninstallJSIInstaller(facebook::jsi::Runtime& rt) {
    // Audio module dies FIRST so the decode thread (and its HealthWatchdog) is
    // joined before VideoFrameQueue and AVSyncCoordinator are destroyed.
    // The watchdog's readMetrics lambda holds raw videoQueue / syncCoordinator
    // pointers; the audio render path holds an atomic AVSyncCoordinator*.
    // Reversing this order would leave a UAF window between video module
    // destruction and audio decode-thread join.
    uninstallMediaPipeline(rt);
    uninstallVideoPipeline(rt);
}
