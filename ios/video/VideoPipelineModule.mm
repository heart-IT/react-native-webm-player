#include "VideoPipelineModule.h"
#include "VP9Decoder.h"
#include "../../cpp/common/MediaLog.h"
#include "../../cpp/common/JsiUtils.h"
#include "../../cpp/common/MediaTime.h"
#include "../../cpp/common/ThreadAffinity.h"
#include "../../cpp/video/VideoSurfaceRegistry.h"
#import <AVFoundation/AVFoundation.h>

namespace videomodule {

namespace jsi = facebook::jsi;

bool VideoPipelineModule::install(jsi::Runtime& rt,
                                   std::shared_ptr<facebook::react::CallInvoker> callInvoker) {
    auto& registry = VideoPipelineRegistry::instance();
    uintptr_t rtId = VideoPipelineRegistry::getRuntimeId(rt);

    if (registry.getModule(rtId)) {
        MEDIA_LOG_W("VideoPipelineModule: already installed for this runtime");
        return true;
    }

    auto& surfaceRegistry = media::VideoSurfaceRegistry::instance();
    surfaceRegistry.setSurfaceAcquireFn([](media::VideoSurface s) {
        if (s) CFRetain(s);
    });
    surfaceRegistry.setSurfaceReleaseFn([](media::VideoSurface s) {
        if (s) CFRelease(s);
    });

    auto module = std::make_shared<VideoPipelineModule>();
    module->callInvoker_ = callInvoker;
    module->runtime_ = &rt;
    registry.registerModule(rtId, module);

    MEDIA_LOG_I("VideoPipelineModule: installed (internal, not exposed to JS)");
    return true;
}

void VideoPipelineModule::uninstall(jsi::Runtime& rt) {
    auto& registry = VideoPipelineRegistry::instance();
    uintptr_t rtId = VideoPipelineRegistry::getRuntimeId(rt);

    auto module = registry.getModule(rtId);
    if (!module) return;

    module->stop(rt);
    module->runtime_ = nullptr;

    media::VideoSurfaceRegistry::instance().clear();

    registry.unregisterModule(rtId);

    MEDIA_LOG_I("VideoPipelineModule: uninstalled");
}

VideoPipelineModule::~VideoPipelineModule() { stopDecodeThread(); }

namespace {

using jsiutil::BufferView;
using jsiutil::extractBuffer;

enum class MethodId : uint8_t {
    Unknown = 0, Start, Stop, IsRunning, GetMetrics
};

MethodId lookupMethod(const std::string& name) noexcept {
    if (name == "start") return MethodId::Start;
    if (name == "stop") return MethodId::Stop;
    if (name == "isRunning") return MethodId::IsRunning;
    if (name == "getMetrics") return MethodId::GetMetrics;
    return MethodId::Unknown;
}

}  // namespace

jsi::Value VideoPipelineModule::get(jsi::Runtime& rt, const jsi::PropNameID& name) {
    std::string n = name.utf8(rt);
    switch (lookupMethod(n)) {
        case MethodId::Start:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) {
                    return start(rt);
                });
        case MethodId::Stop:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) {
                    return stop(rt);
                });
        case MethodId::IsRunning:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) {
                    return isRunning(rt);
                });
        case MethodId::GetMetrics:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) {
                    return getMetrics(rt);
                });
        default: return jsi::Value::undefined();
    }
}

std::vector<jsi::PropNameID> VideoPipelineModule::getPropertyNames(jsi::Runtime& rt) {
    std::vector<jsi::PropNameID> names;
    names.reserve(5);
    names.push_back(jsi::PropNameID::forAscii(rt, "start"));
    names.push_back(jsi::PropNameID::forAscii(rt, "stop"));
    names.push_back(jsi::PropNameID::forAscii(rt, "isRunning"));
    names.push_back(jsi::PropNameID::forAscii(rt, "getMetrics"));
    return names;
}

jsi::Value VideoPipelineModule::start(jsi::Runtime&) {
    std::lock_guard<std::mutex> lk(lifecycleMtx_);
    if (running_.load(std::memory_order_acquire)) return jsi::Value(true);
    running_.store(true, std::memory_order_release);
    frameQueue_.reset();
    startDecodeThread();
    MEDIA_LOG_I("VideoPipelineModule: started");
    return jsi::Value(true);
}

jsi::Value VideoPipelineModule::stop(jsi::Runtime&) {
    std::lock_guard<std::mutex> lk(lifecycleMtx_);
    if (!running_.load(std::memory_order_acquire)) return jsi::Value(true);
    running_.store(false, std::memory_order_release);
    stopDecodeThread();
    frameQueue_.reset();
    syncCoordinator_.reset();
    MEDIA_LOG_I("VideoPipelineModule: stopped");
    return jsi::Value(true);
}

jsi::Value VideoPipelineModule::isRunning(jsi::Runtime&) {
    return jsi::Value(running_.load(std::memory_order_acquire));
}

jsi::Value VideoPipelineModule::getMetrics(jsi::Runtime& rt) {
    jsi::Object result(rt);
    result.setProperty(rt, "avSyncOffsetUs", static_cast<double>(syncCoordinator_.currentOffsetUs()));

    const auto& m = frameQueue_.metrics();
    result.setProperty(rt, "framesReceived", static_cast<double>(m.framesReceived.load(std::memory_order_relaxed)));
    result.setProperty(rt, "framesDecoded", static_cast<double>(m.framesDecoded.load(std::memory_order_relaxed)));
    result.setProperty(rt, "framesDropped", static_cast<double>(m.framesDropped.load(std::memory_order_relaxed)));
    result.setProperty(rt, "decodeErrors", static_cast<double>(m.decodeErrors.load(std::memory_order_relaxed)));
    result.setProperty(rt, "currentFps", m.currentFps.load(std::memory_order_relaxed));
    result.setProperty(rt, "width", m.width.load(std::memory_order_relaxed));
    result.setProperty(rt, "height", m.height.load(std::memory_order_relaxed));
    result.setProperty(rt, "jitterUs", static_cast<double>(m.jitterUs.load(std::memory_order_relaxed)));
    result.setProperty(rt, "bufferTargetUs", static_cast<double>(m.bufferTargetUs.load(std::memory_order_relaxed)));
    result.setProperty(rt, "lateFrames", static_cast<double>(m.lateFrames.load(std::memory_order_relaxed)));
    result.setProperty(rt, "skippedFrames", static_cast<double>(m.skippedFrames.load(std::memory_order_relaxed)));
    result.setProperty(rt, "decoderResets", static_cast<double>(m.decoderResets.load(std::memory_order_relaxed)));
    result.setProperty(rt, "surfaceLostCount", static_cast<double>(m.surfaceLostCount.load(std::memory_order_relaxed)));
    result.setProperty(rt, "maxInterFrameGapMs", static_cast<double>(m.maxInterFrameGapUs.load(std::memory_order_relaxed)) / 1000.0);
    result.setProperty(rt, "gapsOver50ms", static_cast<double>(m.gapsOver50ms.load(std::memory_order_relaxed)));
    result.setProperty(rt, "gapsOver100ms", static_cast<double>(m.gapsOver100ms.load(std::memory_order_relaxed)));
    result.setProperty(rt, "gapsOver500ms", static_cast<double>(m.gapsOver500ms.load(std::memory_order_relaxed)));
    result.setProperty(rt, "driftPpm", static_cast<double>(m.driftPpm.load(std::memory_order_relaxed)));
    result.setProperty(rt, "queueDepth", static_cast<int>(frameQueue_.pendingFrames()));
    result.setProperty(rt, "needsKeyFrame", frameQueue_.isAwaitingKeyFrame());
    return result;
}

void VideoPipelineModule::startDecodeThread() {
    if (decodeRunning_.load(std::memory_order_acquire)) return;
    decodeThreadExited_.store(false, std::memory_order_relaxed);
    wakeVersion_.store(0, std::memory_order_relaxed);
    frameQueue_.setWakeCallback([this]() { wakeDecodeThread(); });
    frameQueue_.setResetCallback([this]() {
        pendingClockReset_.store(true, std::memory_order_release);
        wakeDecodeThread();
    });
    decodeRunning_.store(true, std::memory_order_release);
    decodeThread_ = std::thread(&VideoPipelineModule::decodeLoop, this);
}

void VideoPipelineModule::stopDecodeThread() {
    decodeRunning_.store(false, std::memory_order_release);
    wakeDecodeThread();
    frameQueue_.setWakeCallback(nullptr);
    frameQueue_.setResetCallback(nullptr);
    media::timedJoin(decodeThread_, decodeThreadExited_, media::config::thread::kThreadJoinTimeoutMs, "VideoDecodeThread");

    if (!decodeThreadExited_.load(std::memory_order_acquire)) {
        MEDIA_LOG_E("VideoPipelineModule: decode thread detached, skipping resource destruction");
        return;
    }

    std::lock_guard<std::mutex> lk(decoderMtx_);
    decoder_.reset();
    fpsCounter_.reset();
    renderClock_.reset();
    lastAnchorTimeUs_ = 0;
    consecutiveErrors_ = 0;
    pendingClockReset_.store(false, std::memory_order_relaxed);
}

void VideoPipelineModule::decodeLoop() {
    media::thread_affinity::setThreadName("VP9Decode");
    media::thread_affinity::configureCurrentThreadForDecode();

    media::EncodedVideoFrame frame;
    int consecutiveIdleCycles = 0;
    uint32_t lastWakeVersion = 0;

    while (decodeRunning_.load(std::memory_order_acquire)) {
        bool didWork = false;

        if (pendingClockReset_.exchange(false, std::memory_order_acquire)) {
            renderClock_.reset();
            lastAnchorTimeUs_ = 0;
            syncCoordinator_.reset();
        }

        if (!media::VideoSurfaceRegistry::instance().hasSurface()) {
            std::lock_guard<std::mutex> lk(decoderMtx_);
            decoder_.reset();
            renderClock_.reset();
            lastAnchorTimeUs_ = 0;
        } else {
            int64_t now = media::nowUs();

            // Anchor render clock on first frame
            if (!renderClock_.isAnchored()) {
                int64_t nextPts;
                if (frameQueue_.peekNextPts(nextPts)) {
                    int64_t adjustedTarget = frameQueue_.bufferTargetUs() + syncCoordinator_.videoBufferAdjustmentUs();
                    adjustedTarget = std::clamp(adjustedTarget,
                                                media::video_config::jitter::kMinBufferUs,
                                                media::video_config::jitter::kMaxBufferUs);
                    renderClock_.anchor(nextPts, now, adjustedTarget);
                    lastAnchorTimeUs_ = now;
                }
            } else if (lastAnchorTimeUs_ > 0 &&
                       (now - lastAnchorTimeUs_) >= media::video_config::kReanchorIntervalUs) {
                renderClock_.reset();
                lastAnchorTimeUs_ = 0;
            }

            // Update buffer target with A/V sync correction
            int64_t adjustedTarget = frameQueue_.bufferTargetUs() + syncCoordinator_.videoBufferAdjustmentUs();
            adjustedTarget = std::clamp(adjustedTarget,
                                        media::video_config::jitter::kMinBufferUs,
                                        media::video_config::jitter::kMaxBufferUs);
            renderClock_.updateBufferTarget(adjustedTarget);
            renderClock_.setPlaybackRate(frameQueue_.playbackRate());

            // Update jitter metrics
            frameQueue_.metrics().jitterUs.store(frameQueue_.jitterUs(), std::memory_order_relaxed);
            frameQueue_.metrics().bufferTargetUs.store(frameQueue_.bufferTargetUs(), std::memory_order_relaxed);

            // Pop and decode
            auto popResult = frameQueue_.tryPopReadyFrame(frame, renderClock_, now);
            if (popResult != media::PopResult::Nothing) {
                @autoreleasepool {
                    std::lock_guard<std::mutex> lk(decoderMtx_);
                    if (!decoder_) {
                        auto surface = media::VideoSurfaceRegistry::instance().acquireSurface();
                        if (surface) {
                            auto d = std::make_unique<VP9Decoder>();
                            d->setOutputLayer((__bridge AVSampleBufferDisplayLayer*)surface);
                            CFRelease(surface);
                            d->setKeyFrameRequestFn([]() {
                                media::VideoSurfaceRegistry::instance().requestKeyFrame();
                            });
                            decoder_ = std::move(d);
                        }
                    }

                    if (decoder_) {
                        if (decoder_->submitFrame(frame.data.data(), frame.data.size(),
                                                  frame.ptsUs, frame.isKeyFrame)) {
                            consecutiveErrors_ = 0;
                            frameQueue_.metrics().framesDecoded.fetch_add(1, std::memory_order_relaxed);
                            frameQueue_.metrics().currentFps.store(
                                fpsCounter_.tick(media::nowUs()), std::memory_order_relaxed);
                            if (decoder_->decodedWidth() > 0) {
                                frameQueue_.metrics().width.store(decoder_->decodedWidth(), std::memory_order_relaxed);
                                frameQueue_.metrics().height.store(decoder_->decodedHeight(), std::memory_order_relaxed);
                                media::VideoSurfaceRegistry::instance().setDecodedResolution(
                                    decoder_->decodedWidth(), decoder_->decodedHeight());
                            }
                            syncCoordinator_.onVideoRender(frame.ptsUs, media::nowUs());
                        } else {
                            frameQueue_.metrics().decodeErrors.fetch_add(1, std::memory_order_relaxed);
                            if (++consecutiveErrors_ >= kMaxConsecutiveErrors) {
                                MEDIA_LOG_E("VideoPipelineModule: %d consecutive errors, resetting decoder", consecutiveErrors_);
                                decoder_.reset();
                                renderClock_.reset();
                                lastAnchorTimeUs_ = 0;
                                frameQueue_.requestKeyFrame();
                                frameQueue_.metrics().decoderResets.fetch_add(1, std::memory_order_relaxed);
                                consecutiveErrors_ = 0;
                            }
                        }
                    }
                }
                didWork = true;
            }
        }

        if (!didWork) {
            consecutiveIdleCycles++;
            int64_t sleepUs;
            if (consecutiveIdleCycles < 4) {
                sleepUs = media::video_config::kDecodeIdleSleepUs;
            } else {
                // Cap at shift 3 (max 16ms) to maintain 2:1 margin over 30fps frame interval
                int backoffShift = std::min(consecutiveIdleCycles - 4, 3);
                sleepUs = media::video_config::kDecodeIdleSleepUs << backoffShift;
            }
            waitForWakeOrTimeout(lastWakeVersion, sleepUs);
            lastWakeVersion = wakeVersion_.load(std::memory_order_relaxed);
        } else {
            consecutiveIdleCycles = 0;
            lastWakeVersion = wakeVersion_.load(std::memory_order_relaxed);
        }
    }

    decodeThreadExited_.store(true, std::memory_order_release);
}

}  // namespace videomodule
