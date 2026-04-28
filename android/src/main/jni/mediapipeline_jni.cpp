#include <jsi/jsi.h>
#include <ReactCommon/CallInvoker.h>
#include "playback/MediaPipelineModule.h"

extern "C" bool installMediaPipeline(facebook::jsi::Runtime& rt,
                                     std::shared_ptr<facebook::react::CallInvoker> callInvoker) {
    return mediamodule::MediaPipelineModule::install(rt, callInvoker);
}

extern "C" void uninstallMediaPipeline(facebook::jsi::Runtime& rt) {
    mediamodule::MediaPipelineModule::uninstall(rt);
}
