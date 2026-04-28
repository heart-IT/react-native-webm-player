// Android media session — thin typedef over shared MediaSessionBase template.
#pragma once

#include "common/MediaSessionBase.h"
#include "AAudioOutputBridge.h"
#include "AudioSessionManager.h"

namespace media {

struct AndroidAudioTraits {
    using Bridge = AAudioOutputBridge;
    using BridgeConfig = AAudioConfig;

    static AudioSessionManager& managerInstance() noexcept {
        return AudioSessionManager::instance();
    }

    static void shutdownManager() noexcept {
        AudioSessionManager::instance().shutdown(nullptr);
    }
};

using SessionConfig = MediaSessionBase<AndroidAudioTraits>::Config;
using MediaSession = MediaSessionBase<AndroidAudioTraits>;

}  // namespace media
