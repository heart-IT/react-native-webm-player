// iOS media session — thin typedef over shared MediaSessionBase template.
#pragma once

#include "../../cpp/common/MediaSessionBase.h"
#include "AVAudioOutputBridge.h"
#include "AudioSessionManager.h"

namespace media {

struct IOSAudioTraits {
    using Bridge = AVAudioOutputBridge;
    using BridgeConfig = AudioOutputConfig;

    static AudioSessionManager& managerInstance() noexcept {
        return AudioSessionManager::instance();
    }

    static void shutdownManager() noexcept {
        AudioSessionManager::instance().shutdown();
    }
};

using SessionConfig = MediaSessionBase<IOSAudioTraits>::Config;
using MediaSession = MediaSessionBase<IOSAudioTraits>;

}  // namespace media
