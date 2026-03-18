#import "AVAudioOutputBridge.h"
#import "AudioSessionManager.h"
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>

namespace media {

// Static C render callback for RemoteIO output - mirrors capture bridge pattern.
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

AVAudioOutputBridge::AVAudioOutputBridge(AudioCallback callback, AVAudioConfig config) noexcept
    : callback_(std::move(callback))
    , config_(config) {}

AVAudioOutputBridge::~AVAudioOutputBridge() noexcept {
    stop();
}

void AVAudioOutputBridge::setRestartCallback(RestartCallback cb) noexcept {
    std::lock_guard<std::mutex> lk(callbackMtx_);
    restartCallback_ = std::move(cb);
}

bool AVAudioOutputBridge::warmUp() noexcept {
    std::lock_guard<std::mutex> lk(lifecycleMtx_);

    uint8_t expected = kStateStopped;
    if (!callbackState_.compare_exchange_strong(expected, kStateWarmUp,
            std::memory_order_acq_rel, std::memory_order_relaxed)) {
        // Already warming up or active - success
        uint8_t current = callbackState_.load(std::memory_order_acquire);
        return current == kStateWarmUp || current == kStateActive;
    }

    if (!createAudioUnit()) {
        callbackState_.store(kStateStopped, std::memory_order_release);
        return false;
    }

    restartThreadExited_.store(false, std::memory_order_relaxed);
    restartThread_ = std::thread([this] { restartLoop(); });

    if (!startAudioUnit()) {
        restartRequested_.store(true, std::memory_order_release);
        restartRequested_.notify_one();
        callbackState_.store(kStateShuttingDown, std::memory_order_release);
        timedJoin(restartThread_, restartThreadExited_,
                  config::thread::kThreadJoinTimeoutMs, "AVAudioOutputBridge::warmUp");
        destroyAudioUnit();
        callbackState_.store(kStateStopped, std::memory_order_release);
        return false;
    }

    consecutiveRestartFailures_.store(0, std::memory_order_relaxed);
    MEDIA_LOG_I("AVAudioOutputBridge: warm-up complete, hardware hot");
    return true;
}

bool AVAudioOutputBridge::start() noexcept {
    std::lock_guard<std::mutex> lk(lifecycleMtx_);

    uint8_t expected = kStateWarmUp;

    // If already warmed up, transition to active (CAS to avoid racing with error callback)
    if (callbackState_.compare_exchange_strong(expected, kStateActive,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        MEDIA_LOG_I("AVAudioOutputBridge: warm-up -> active (zero latency transition)");
        return true;
    }

    // If already active, nothing to do
    if (expected == kStateActive) {
        return true;
    }

    // Cold start path - full initialization
    expected = kStateStopped;
    if (!callbackState_.compare_exchange_strong(expected, kStateActive,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return false;
    }

    if (!createAudioUnit()) {
        callbackState_.store(kStateStopped, std::memory_order_release);
        return false;
    }

    restartThreadExited_.store(false, std::memory_order_relaxed);
    restartThread_ = std::thread([this] { restartLoop(); });

    if (!startAudioUnit()) {
        // Signal restart thread to exit before cleanup
        restartRequested_.store(true, std::memory_order_release);
        restartRequested_.notify_one();

        // Transition to shutting down so restart thread exits its loop
        callbackState_.store(kStateShuttingDown, std::memory_order_release);

        timedJoin(restartThread_, restartThreadExited_,
                  config::thread::kThreadJoinTimeoutMs, "AVAudioOutputBridge::start");

        destroyAudioUnit();
        callbackState_.store(kStateStopped, std::memory_order_release);
        return false;
    }

    consecutiveRestartFailures_.store(0, std::memory_order_relaxed);
    return true;
}

void AVAudioOutputBridge::stop() noexcept {
    {
        std::lock_guard<std::mutex> lk(lifecycleMtx_);
        uint8_t current = callbackState_.load(std::memory_order_acquire);

        // Already stopped, nothing to do
        if (current == kStateStopped) return;

        // Transition from warm-up or active to shutting down
        if (current == kStateWarmUp || current == kStateActive) {
            callbackState_.store(kStateShuttingDown, std::memory_order_release);
        } else if (current == kStateShuttingDown) {
            // Already shutting down
            return;
        }
    }

    restartRequested_.store(true, std::memory_order_release);
    restartRequested_.notify_one();

    timedJoin(restartThread_, restartThreadExited_,
              config::thread::kThreadJoinTimeoutMs, "AVAudioOutputBridge::stop");
    destroyAudioUnit();

    callbackState_.store(kStateStopped, std::memory_order_release);
}

void AVAudioOutputBridge::scheduleRestart() noexcept {
    restartRequested_.store(true, std::memory_order_release);
    restartRequested_.notify_one();
}

size_t AVAudioOutputBridge::renderAudio(float* output, size_t frameCount) noexcept {
    // Track in-flight callbacks for safe shutdown.
    // notify_one on decrement-to-zero wakes waitForCallbackDrain() without
    // adding a syscall in the common case (counter > 1 after subtract).
    callbackInflight_.fetch_add(1, std::memory_order_relaxed);
    struct CallbackGuard {
        std::atomic<int32_t>& counter;
        ~CallbackGuard() {
            if (counter.fetch_sub(1, std::memory_order_release) == 1) {
                counter.notify_one();
            }
        }
    } guard{callbackInflight_};

    if (UNLIKELY(!output || frameCount == 0)) {
        return 0;
    }

    uint8_t state = callbackState_.load(std::memory_order_acquire);
    if (state == kStateWarmUp) {
        // Warm-up mode: output silence to keep hardware hot
        std::memset(output, 0, frameCount * config_.channelCount * sizeof(float));
        return frameCount;  // Report frames written to keep Audio Unit happy
    }
    // Interruption state via cached pointer (avoids Meyers-singleton guard on RT path)
    auto* mgr = sessionMgr_.load(std::memory_order_acquire);
    if (UNLIKELY(!mgr || !mgr->canPlayAudio())) {
        std::memset(output, 0, frameCount * config_.channelCount * sizeof(float));
        return frameCount;
    }
    if (UNLIKELY(state != kStateActive)) {
        std::memset(output, 0, frameCount * config_.channelCount * sizeof(float));
        return 0;
    }

    size_t written = 0;
    if (LIKELY(static_cast<bool>(callback_))) {
        if (LIKELY(playbackResampler_.isPassthrough())) {
            // Passthrough: hardware rate == 48kHz, write directly
            written = callback_(output, frameCount);
        } else {
            // Resample path: mixer outputs at 48kHz, hardware needs different rate.
            // Compute how many 48kHz source frames produce `frameCount` hardware frames.
            // srcRate=48000, dstRate=hwRate. We need `frameCount` output frames.
            // srcFrames = frameCount * srcRate / dstRate (ceiling)
            int32_t hwRate = grantedSampleRate_.load(std::memory_order_relaxed);
            if (UNLIKELY(hwRate <= 0)) hwRate = config_.sampleRate;

            size_t srcFrames = (frameCount * static_cast<size_t>(config_.sampleRate)
                                + static_cast<size_t>(hwRate) - 1)
                               / static_cast<size_t>(hwRate);
            // Extra source frame absorbs Speex filter delay to prevent
            // silence-pad underruns on Bluetooth SCO (8/16kHz)
            srcFrames += 1;

            // Clamp to buffer capacity
            if (UNLIKELY(srcFrames > resampleSrcBuffer_.size())) {
                srcFrames = resampleSrcBuffer_.size();
            }

            // Get mixer audio at 48kHz into source buffer
            written = callback_(resampleSrcBuffer_.data(), srcFrames);

            if (LIKELY(written > 0)) {
                // Resample from 48kHz → hardware rate.
                // Clamp output to frameCount to prevent off-by-one from ceiling division.
                size_t resampledCount = playbackResampler_.process(
                    output, resampleSrcBuffer_.data(), written);
                written = std::min(resampledCount, frameCount);
            }
        }
    }

    if (LIKELY(written > 0)) {
        framesWritten_.fetch_add(static_cast<int64_t>(written), std::memory_order_relaxed);
    }

    if (written < frameCount) {
        size_t remaining = (frameCount - written) * config_.channelCount;
        std::memset(output + written * config_.channelCount, 0, remaining * sizeof(float));
    }

    return written;
}

bool AVAudioOutputBridge::createAudioUnit() noexcept {
    @autoreleasepool {
        NSError* error = nil;

        AVAudioSession* session = [AVAudioSession sharedInstance];

        // Cache AudioSessionManager reference for RT callback path
        // (avoids Meyers-singleton guard lock). Lifecycle (initialize/shutdown)
        // is owned by MediaSession — bridges must not bump refCount.
        auto& mgr = AudioSessionManager::instance();
        sessionMgr_.store(&mgr, std::memory_order_release);

        BOOL success;

        // Request low-latency buffer duration
        NSTimeInterval preferredBufferDuration = static_cast<double>(config_.framesPerBuffer) /
                                                  static_cast<double>(config_.sampleRate);
        success = [session setPreferredIOBufferDuration:preferredBufferDuration error:&error];
        if (!success && error) {
            MEDIA_LOG_W("AVAudioSession setPreferredIOBufferDuration failed: %s",
                        [[error localizedDescription] UTF8String]);
        }

        success = [session setPreferredSampleRate:static_cast<double>(config_.sampleRate) error:&error];
        if (!success && error) {
            MEDIA_LOG_W("AVAudioSession setPreferredSampleRate failed: %s",
                        [[error localizedDescription] UTF8String]);
        }

        // setActive:YES is handled by MediaSession which owns AudioSessionManager lifecycle

        NSTimeInterval actualBufferDuration = session.IOBufferDuration;
        if (actualBufferDuration <= ios_audio::kLowLatencyThresholdSec) {
            grantedLatencyMode_.store(LatencyMode::LowLatency, std::memory_order_relaxed);
        } else {
            grantedLatencyMode_.store(LatencyMode::Standard, std::memory_order_relaxed);
        }

        // Read hardware sample rate and initialize resampler if needed
        double grantedRate = session.sampleRate;
        int32_t hwRate = static_cast<int32_t>(grantedRate);
        grantedSampleRate_.store(hwRate, std::memory_order_relaxed);
        sampleRateValid_.store(true, std::memory_order_relaxed);

        // Initialize resampler: mixer outputs at 48kHz, hardware may run at different rate
        if (!playbackResampler_.init(config_.sampleRate, hwRate)) {
            MEDIA_LOG_E("AVAudioOutputBridge: resampler init failed (%d -> %d Hz)",
                        config_.sampleRate, hwRate);
            lastError_.store(-3, std::memory_order_relaxed);
            return false;
        }

        if (hwRate != config_.sampleRate) {
            MEDIA_LOG_I("AVAudioOutputBridge: resampling %d -> %d Hz",
                        config_.sampleRate, hwRate);
        }

        // Create RemoteIO Audio Unit for output
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
                                       0, // Output bus
                                       &enableOutput,
                                       sizeof(enableOutput));
        if (status != noErr) {
            MEDIA_LOG_E("AVAudioOutputBridge: enable output failed: %d", static_cast<int>(status));
            AudioComponentInstanceDispose(audioUnit);
            return false;
        }

        // Disable input on bus 1 (microphone) - capture is handled separately by VoiceProcessingIO
        UInt32 enableInput = 0;
        status = AudioUnitSetProperty(audioUnit,
                                       kAudioOutputUnitProperty_EnableIO,
                                       kAudioUnitScope_Input,
                                       1, // Input bus
                                       &enableInput,
                                       sizeof(enableInput));
        if (status != noErr) {
            MEDIA_LOG_W("AVAudioOutputBridge: disable input failed: %d (non-fatal)", static_cast<int>(status));
        }

        // Set output format: float32 mono at hardware sample rate.
        // We feed pre-resampled audio at hardware rate from the render callback.
        AudioStreamBasicDescription outputFormat = {
            .mSampleRate = grantedRate,
            .mFormatID = kAudioFormatLinearPCM,
            .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
            .mBytesPerPacket = sizeof(float),
            .mFramesPerPacket = 1,
            .mBytesPerFrame = sizeof(float),
            .mChannelsPerFrame = 1,
            .mBitsPerChannel = 32,
            .mReserved = 0
        };

        // Set format on input scope of output bus (what we provide to hardware)
        status = AudioUnitSetProperty(audioUnit,
                                       kAudioUnitProperty_StreamFormat,
                                       kAudioUnitScope_Input,
                                       0, // Output bus
                                       &outputFormat,
                                       sizeof(outputFormat));
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
                                       0, // Output bus
                                       &callbackStruct,
                                       sizeof(callbackStruct));
        if (status != noErr) {
            MEDIA_LOG_E("AVAudioOutputBridge: set render callback failed: %d", static_cast<int>(status));
            AudioComponentInstanceDispose(audioUnit);
            return false;
        }

        // Constrain MaximumFramesPerSlice BEFORE AudioUnitInitialize.
        // resampleSrcBuffer_ (kResampleSrcCapacity = 24576) handles up to 4096 hw frames
        // at 6x upsample ratio (8kHz→48kHz). The render callback clamps srcFrames to
        // buffer capacity as defense-in-depth.
        UInt32 requestedMaxFrames = 4096;
        status = AudioUnitSetProperty(audioUnit,
                                       kAudioUnitProperty_MaximumFramesPerSlice,
                                       kAudioUnitScope_Global,
                                       0,
                                       &requestedMaxFrames,
                                       sizeof(requestedMaxFrames));
        if (status != noErr) {
            MEDIA_LOG_W("AVAudioOutputBridge: set MaxFramesPerSlice failed: %d (non-fatal)",
                        static_cast<int>(status));
        }

        // Initialize the Audio Unit
        status = AudioUnitInitialize(audioUnit);
        if (status != noErr) {
            MEDIA_LOG_E("AVAudioOutputBridge: AudioUnitInitialize failed: %d", static_cast<int>(status));
            AudioComponentInstanceDispose(audioUnit);
            return false;
        }

        // Query MaximumFramesPerSlice after init to log the negotiated value.
        // iOS may grant a larger value than requested (common during BT SCO).
        // This is safe because:
        // - resampleSrcBuffer_ (24576 samples) handles 4096 hw frames at up to 6x ratio
        // - renderAudio() clamps srcFrames to buffer capacity as defense-in-depth
        UInt32 maxFrames = 0;
        UInt32 maxFramesSize = sizeof(maxFrames);
        status = AudioUnitGetProperty(audioUnit,
                                       kAudioUnitProperty_MaximumFramesPerSlice,
                                       kAudioUnitScope_Global,
                                       0,
                                       &maxFrames,
                                       &maxFramesSize);
        if (status == noErr) {
            MEDIA_LOG_I("AVAudioOutputBridge: MaxFramesPerSlice=%u (requested=%u)",
                        static_cast<unsigned>(maxFrames), static_cast<unsigned>(requestedMaxFrames));
        } else {
            MEDIA_LOG_W("AVAudioOutputBridge: failed to query MaxFramesPerSlice: %d",
                        static_cast<int>(status));
        }

        audioUnit_ = audioUnit;
        audioUnitActive_.store(true, std::memory_order_release);

        // Cache and report measured output latency.
        // Cached value makes totalLatencyUs() callable from any thread without ObjC.
        int64_t measuredLatencyUs = static_cast<int64_t>(
            (session.outputLatency + session.IOBufferDuration) * 1000000.0);
        cachedLatencyUs_.store(measuredLatencyUs, std::memory_order_relaxed);
        AudioSessionManager::instance().updateMeasuredOutputLatency(measuredLatencyUs);

        double actualOutputLatency = session.outputLatency * 1000.0;
        double actualIOBufferDuration = session.IOBufferDuration * 1000.0;

        MEDIA_LOG_I("AVAudioOutputBridge: RemoteIO created (hwRate=%d resample=%s latency=%.1fms buffer=%.1fms mode=%s)",
                    hwRate,
                    playbackResampler_.isPassthrough() ? "none" : "active",
                    actualOutputLatency, actualIOBufferDuration,
                    latencyModeToString(grantedLatencyMode_.load(std::memory_order_relaxed)));

        return true;
    }
}

bool AVAudioOutputBridge::startAudioUnit() noexcept {
    @autoreleasepool {
        if (!audioUnit_) {
            MEDIA_LOG_E("AVAudioOutputBridge: startAudioUnit: no audio unit");
            return false;
        }

        AudioUnit audioUnit = audioUnit_;
        OSStatus status = AudioOutputUnitStart(audioUnit);
        if (status != noErr) {
            MEDIA_LOG_E("AVAudioOutputBridge: AudioOutputUnitStart failed: %d", static_cast<int>(status));
            lastError_.store(static_cast<int>(status), std::memory_order_relaxed);
            return false;
        }

        MEDIA_LOG_I("AVAudioOutputBridge: Audio Unit started");
        return true;
    }
}

void AVAudioOutputBridge::waitForCallbackDrain() noexcept {
    // Wait for all in-flight audio callbacks to complete before releasing resources.
    // Uses C++20 atomic::wait() instead of sleep-polling: the CallbackGuard destructor
    // calls notify_one() on decrement-to-zero, waking this waiter efficiently.
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::microseconds(ios_audio::kCallbackDrainTimeoutUs);
    while (callbackInflight_.load(std::memory_order_acquire) > 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            MEDIA_LOG_W("AVAudioOutputBridge: callback drain timeout (%d still in-flight)",
                        callbackInflight_.load(std::memory_order_relaxed));
            break;
        }
        callbackInflight_.wait(callbackInflight_.load(std::memory_order_relaxed),
                               std::memory_order_acquire);
    }
}

void AVAudioOutputBridge::destroyAudioUnit() noexcept {
    @autoreleasepool {
        if (audioUnit_) {
            AudioUnit audioUnit = audioUnit_;

            // Stop new callbacks from being scheduled
            AudioOutputUnitStop(audioUnit);

            // Wait for any in-flight callbacks to complete
            waitForCallbackDrain();

            AudioUnitUninitialize(audioUnit);
            AudioComponentInstanceDispose(audioUnit);
            audioUnit_ = nullptr;
            audioUnitActive_.store(false, std::memory_order_release);
            cachedLatencyUs_.store(0, std::memory_order_relaxed);
        }

        // Reset sample rate validation state
        sampleRateValid_.store(false, std::memory_order_relaxed);
        grantedSampleRate_.store(0, std::memory_order_relaxed);

        // Destroy resampler state
        playbackResampler_.destroy();
    }
}

void AVAudioOutputBridge::restartLoop() noexcept {
    thread_affinity::setThreadName("AVAudioOutRstrt");
    thread_affinity::configureCurrentThreadForWorker();

    while (callbackState_.load(std::memory_order_acquire) != kStateShuttingDown) {
        // Wait for restart request using atomic wait
        while (!restartRequested_.load(std::memory_order_acquire)) {
            if (callbackState_.load(std::memory_order_acquire) == kStateShuttingDown) {
                restartThreadExited_.store(true, std::memory_order_release);
                return;
            }
            restartRequested_.wait(false, std::memory_order_relaxed);
            // Recheck shutdown after wake — notification from stop() may have arrived
            // between the state check above and the wait() call
            if (callbackState_.load(std::memory_order_acquire) == kStateShuttingDown) {
                restartThreadExited_.store(true, std::memory_order_release);
                return;
            }
        }

        if (callbackState_.load(std::memory_order_acquire) == kStateShuttingDown) break;
        restartRequested_.store(false, std::memory_order_release);

        {
            // Serialize audioUnit_ access with stop()/warmUp()/start() on the JS thread.
            std::lock_guard<std::mutex> lk(lifecycleMtx_);

            uint8_t state = callbackState_.load(std::memory_order_acquire);
            if (state != kStateActive && state != kStateWarmUp) continue;

            destroyAudioUnit();
        }

        // Brief delay before restart — outside lock so stop() can proceed promptly
        std::this_thread::sleep_for(std::chrono::milliseconds(ios_audio::kRestartDelayMs));

        {
            std::unique_lock<std::mutex> lk(lifecycleMtx_);

            // Re-check state: stop() may have transitioned to kStateShuttingDown during sleep
            uint8_t state = callbackState_.load(std::memory_order_acquire);
            if (state != kStateActive && state != kStateWarmUp) continue;

            if (!createAudioUnit() || !startAudioUnit()) {
                int failures = consecutiveRestartFailures_.fetch_add(1, std::memory_order_relaxed) + 1;

                if (failures >= kMaxRestartRetries) {
                    MEDIA_LOG_E("AVAudioOutputBridge: max restart retries (%d) exceeded, stopping", kMaxRestartRetries);
                    lastError_.store(-20, std::memory_order_relaxed);
                    callbackState_.store(kStateStopped, std::memory_order_release);

                    // Notify caller that recovery failed
                    RestartCallback failCb;
                    {
                        std::lock_guard<std::mutex> cbLk(callbackMtx_);
                        failCb = restartCallback_;
                    }
                    lk.unlock();
                    if (failCb) failCb();
                    break;
                }

                int backoffMs = std::min(ios_audio::kRestartDelayMs << failures, kMaxBackoffMs);
                MEDIA_LOG_W("AVAudioOutputBridge: restart failed (%d/%d), backoff %dms",
                            failures, kMaxRestartRetries, backoffMs);

                // Release lock during backoff sleep to unblock stop()
                lk.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
                continue;
            }

            consecutiveRestartFailures_.store(0, std::memory_order_relaxed);
            restartCount_.fetch_add(1, std::memory_order_relaxed);
            MEDIA_LOG_I("AVAudioOutputBridge: restart #%u successful",
                        restartCount_.load(std::memory_order_relaxed));
        }

        RestartCallback cb;
        {
            std::lock_guard<std::mutex> lk(callbackMtx_);
            cb = restartCallback_;
        }
        if (cb) cb();
    }
    restartThreadExited_.store(true, std::memory_order_release);
}

}  // namespace media
