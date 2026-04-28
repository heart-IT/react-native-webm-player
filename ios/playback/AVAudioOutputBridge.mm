#import "AVAudioOutputBridge.h"
#import "AudioSessionManager.h"
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>

namespace media {

// Static C render callback for RemoteIO output.
// inRefCon holds AVAudioOutputBridge*. Safety guaranteed by callbackInflight_ counter.
static OSStatus outputRenderCallback(void* inRefCon,
                                      AudioUnitRenderActionFlags* ioActionFlags,
                                      const AudioTimeStamp* inTimeStamp,
                                      UInt32 inBusNumber,
                                      UInt32 inNumberFrames,
                                      AudioBufferList* ioData) {
    auto* bridge = static_cast<AVAudioOutputBridge*>(inRefCon);

    for (UInt32 bufIdx = 0; bufIdx < ioData->mNumberBuffers; ++bufIdx) {
        AudioBuffer& buffer = ioData->mBuffers[bufIdx];
        float* output = static_cast<float*>(buffer.mData);
        size_t frames = static_cast<size_t>(inNumberFrames);

        size_t written = bridge->renderAudio(output, frames);

        if (written == 0) {
            *ioActionFlags |= kAudioUnitRenderAction_OutputIsSilence;
        }
    }

    return noErr;
}

AVAudioOutputBridge::AVAudioOutputBridge(AudioCallback callback, AudioOutputConfig config) noexcept
    : AudioOutputBridgeBase(std::move(callback), config) {}

AVAudioOutputBridge::~AVAudioOutputBridge() noexcept {
    stop();
}

size_t AVAudioOutputBridge::renderAudio(float* output, size_t frameCount) noexcept {
    callbackInflight_.fetch_add(1, std::memory_order_acquire);
    struct CallbackGuard {
        std::atomic<int32_t>& counter;
        ~CallbackGuard() { counter.fetch_sub(1, std::memory_order_release); }
    } guard{callbackInflight_};

    return renderAudioCore(output, frameCount);
}

bool AVAudioOutputBridge::createStreamImpl() noexcept {
    @autoreleasepool {
        NSError* error = nil;
        AVAudioSession* session = [AVAudioSession sharedInstance];

        NSTimeInterval preferredBufferDuration = static_cast<double>(baseConfig_.framesPerBuffer) /
                                                  static_cast<double>(baseConfig_.sampleRate);
        [session setPreferredIOBufferDuration:preferredBufferDuration error:&error];
        [session setPreferredSampleRate:static_cast<double>(baseConfig_.sampleRate) error:&error];

        NSTimeInterval actualBufferDuration = session.IOBufferDuration;
        if (actualBufferDuration <= ios_audio::kLowLatencyThresholdSec) {
            grantedLatencyMode_.store(LatencyMode::LowLatency, std::memory_order_relaxed);
        } else {
            grantedLatencyMode_.store(LatencyMode::Standard, std::memory_order_relaxed);
        }

        double grantedRate = session.sampleRate;
        int32_t hwRate = static_cast<int32_t>(grantedRate);
        grantedSampleRate_.store(hwRate, std::memory_order_relaxed);

        if (!playbackResampler_.init(baseConfig_.sampleRate, hwRate)) {
            MEDIA_LOG_E("AVAudioOutputBridge: resampler init failed (%d -> %d Hz)",
                        baseConfig_.sampleRate, hwRate);
            lastError_.store(-3, std::memory_order_relaxed);
            return false;
        }

        if (hwRate != baseConfig_.sampleRate) {
            MEDIA_LOG_I("AVAudioOutputBridge: resampling %d -> %d Hz",
                        baseConfig_.sampleRate, hwRate);
        }

        // Create RemoteIO Audio Unit
        AudioComponentDescription desc = {
            .componentType = kAudioUnitType_Output,
            .componentSubType = kAudioUnitSubType_RemoteIO,
            .componentManufacturer = kAudioUnitManufacturer_Apple,
            .componentFlags = 0,
            .componentFlagsMask = 0
        };

        AudioComponent component = AudioComponentFindNext(nullptr, &desc);
        if (!component) {
            MEDIA_LOG_E("AVAudioOutputBridge: RemoteIO component not found");
            lastError_.store(-4, std::memory_order_relaxed);
            return false;
        }

        AudioUnit audioUnit = nullptr;
        OSStatus status = AudioComponentInstanceNew(component, &audioUnit);
        if (status != noErr || !audioUnit) {
            MEDIA_LOG_E("AVAudioOutputBridge: AudioComponentInstanceNew failed: %d",
                        static_cast<int>(status));
            lastError_.store(static_cast<int>(status), std::memory_order_relaxed);
            return false;
        }

        // Enable output on bus 0 (speaker)
        UInt32 enableOutput = 1;
        status = AudioUnitSetProperty(audioUnit,
                                       kAudioOutputUnitProperty_EnableIO,
                                       kAudioUnitScope_Output,
                                       0, &enableOutput, sizeof(enableOutput));
        if (status != noErr) {
            MEDIA_LOG_E("AVAudioOutputBridge: enable output failed: %d", static_cast<int>(status));
            AudioComponentInstanceDispose(audioUnit);
            return false;
        }

        // Disable input on bus 1 (microphone) — playback-only
        UInt32 enableInput = 0;
        AudioUnitSetProperty(audioUnit,
                             kAudioOutputUnitProperty_EnableIO,
                             kAudioUnitScope_Input,
                             1, &enableInput, sizeof(enableInput));

        // Set output format: float32 at hardware sample rate
        const UInt32 channels = static_cast<UInt32>(this->baseConfig_.channelCount);
        AudioStreamBasicDescription outputFormat = {
            .mSampleRate = grantedRate,
            .mFormatID = kAudioFormatLinearPCM,
            .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
            .mBytesPerPacket = static_cast<UInt32>(channels * sizeof(float)),
            .mFramesPerPacket = 1,
            .mBytesPerFrame = static_cast<UInt32>(channels * sizeof(float)),
            .mChannelsPerFrame = channels,
            .mBitsPerChannel = 32,
            .mReserved = 0
        };

        status = AudioUnitSetProperty(audioUnit,
                                       kAudioUnitProperty_StreamFormat,
                                       kAudioUnitScope_Input,
                                       0, &outputFormat, sizeof(outputFormat));
        if (status != noErr) {
            MEDIA_LOG_E("AVAudioOutputBridge: set output format failed: %d", static_cast<int>(status));
            AudioComponentInstanceDispose(audioUnit);
            return false;
        }

        // Set render callback
        AURenderCallbackStruct callbackStruct = {
            .inputProc = outputRenderCallback,
            .inputProcRefCon = this
        };

        status = AudioUnitSetProperty(audioUnit,
                                       kAudioUnitProperty_SetRenderCallback,
                                       kAudioUnitScope_Input,
                                       0, &callbackStruct, sizeof(callbackStruct));
        if (status != noErr) {
            MEDIA_LOG_E("AVAudioOutputBridge: set render callback failed: %d", static_cast<int>(status));
            AudioComponentInstanceDispose(audioUnit);
            return false;
        }

        // Constrain MaximumFramesPerSlice
        UInt32 requestedMaxFrames = 4096;
        AudioUnitSetProperty(audioUnit,
                             kAudioUnitProperty_MaximumFramesPerSlice,
                             kAudioUnitScope_Global,
                             0, &requestedMaxFrames, sizeof(requestedMaxFrames));

        // Initialize
        status = AudioUnitInitialize(audioUnit);
        if (status != noErr) {
            MEDIA_LOG_E("AVAudioOutputBridge: AudioUnitInitialize failed: %d", static_cast<int>(status));
            AudioComponentInstanceDispose(audioUnit);
            return false;
        }

        audioUnit_ = audioUnit;
        audioUnitActive_.store(true, std::memory_order_release);

        // Cache latency for RT-safe query
        int64_t measuredLatencyUs = static_cast<int64_t>(
            (session.outputLatency + session.IOBufferDuration) * 1000000.0);
        cachedLatencyUs_.store(measuredLatencyUs, std::memory_order_relaxed);

        MEDIA_LOG_I("AVAudioOutputBridge: RemoteIO created (hwRate=%d resample=%s latency=%.1fms mode=%s)",
                    hwRate,
                    playbackResampler_.isPassthrough() ? "none" : "active",
                    session.outputLatency * 1000.0,
                    latencyModeToString(grantedLatencyMode_.load(std::memory_order_relaxed)));

        return true;
    }
}

bool AVAudioOutputBridge::startStreamImpl() noexcept {
    @autoreleasepool {
        if (!audioUnit_) {
            MEDIA_LOG_E("AVAudioOutputBridge: startStreamImpl: no audio unit");
            return false;
        }

        OSStatus status = AudioOutputUnitStart(audioUnit_);
        if (status != noErr) {
            MEDIA_LOG_E("AVAudioOutputBridge: AudioOutputUnitStart failed: %d", static_cast<int>(status));
            lastError_.store(static_cast<int>(status), std::memory_order_relaxed);
            return false;
        }

        MEDIA_LOG_I("AVAudioOutputBridge: Audio Unit started");
        return true;
    }
}

bool AVAudioOutputBridge::waitForCallbackDrain() noexcept {
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::microseconds(ios_audio::kCallbackDrainTimeoutUs);
    while (callbackInflight_.load(std::memory_order_acquire) > 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            MEDIA_LOG_W("AVAudioOutputBridge: callback drain timeout (%d still in-flight)",
                        callbackInflight_.load(std::memory_order_relaxed));
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

void AVAudioOutputBridge::destroyStreamImpl() noexcept {
    @autoreleasepool {
        if (audioUnit_) {
            AudioOutputUnitStop(audioUnit_);
            bool drained = waitForCallbackDrain();
            if (!drained) {
                // Callback still running after timeout. Disposing now would race
                // with a live render thread reading audioUnit_/resampler state.
                // Leak the unit rather than UAF — the OS will reclaim on process exit.
                MEDIA_LOG_E("AVAudioOutputBridge: leaking AudioUnit — callback did not drain");
                audioUnit_ = nullptr;
                audioUnitActive_.store(false, std::memory_order_release);
                cachedLatencyUs_.store(0, std::memory_order_relaxed);
                grantedSampleRate_.store(0, std::memory_order_relaxed);
                return;
            }
            AudioUnitUninitialize(audioUnit_);
            AudioComponentInstanceDispose(audioUnit_);
            audioUnit_ = nullptr;
            audioUnitActive_.store(false, std::memory_order_release);
            cachedLatencyUs_.store(0, std::memory_order_relaxed);
        }

        grantedSampleRate_.store(0, std::memory_order_relaxed);
        playbackResampler_.destroy();
    }
}

}  // namespace media
