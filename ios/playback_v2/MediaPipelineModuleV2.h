// JSI module for v2 playback engine. Exposes a global __MediaPipelineV2 object
// alongside v1's __MediaPipeline so both can coexist during the migration.
// Once v1 is deleted (Phase 6), rename to __MediaPipeline.
#pragma once

#include <memory>
#include <jsi/jsi.h>
#include <ReactCommon/CallInvoker.h>

@class PlaybackEngineV2;

namespace mediamodule_v2 {

bool installV2JSI(facebook::jsi::Runtime& rt,
                   std::shared_ptr<facebook::react::CallInvoker> callInvoker);
void uninstallV2JSI(facebook::jsi::Runtime& rt);

/// The most recently installed engine. Used by VideoViewV2 to attach its
/// AVSampleBufferDisplayLayer without going through JSI. Returns nil if no
/// engine has been installed (`installWebmPlayer()` not yet called) or after
/// the last runtime is torn down.
PlaybackEngineV2* currentEngine();

}  // namespace mediamodule_v2
