#include "VideoPipelineModule.h"
#include "VP9Decoder.h"
#include "../../cpp/common/MediaLog.h"
#include "../../cpp/common/JsiUtils.h"
#include "../../cpp/common/MediaTime.h"
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

    media::VideoSurfaceRegistry::instance().clear();

    registry.unregisterModule(rtId);

    MEDIA_LOG_I("VideoPipelineModule: uninstalled");
}

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

    // Platform decoder factory: creates VP9Decoder with acquired surface
    auto factory = []() -> std::unique_ptr<media::VideoDecoder> {
        auto surface = media::VideoSurfaceRegistry::instance().acquireSurface();
        if (!surface) return nullptr;

        auto d = std::make_unique<VP9Decoder>();
        d->setOutputLayer((__bridge AVSampleBufferDisplayLayer*)surface);
        CFRelease(surface);
        d->setKeyFrameRequestFn([]() {
            media::VideoSurfaceRegistry::instance().requestKeyFrame();
        });
        return d;
    };

    decodeThread_.start(&frameQueue_, std::move(factory));
    MEDIA_LOG_I("VideoPipelineModule: started");
    return jsi::Value(true);
}

jsi::Value VideoPipelineModule::stop(jsi::Runtime&) {
    std::lock_guard<std::mutex> lk(lifecycleMtx_);
    if (!running_.load(std::memory_order_acquire)) return jsi::Value(true);
    running_.store(false, std::memory_order_release);
    decodeThread_.stop();
    frameQueue_.reset();
    MEDIA_LOG_I("VideoPipelineModule: stopped");
    return jsi::Value(true);
}

jsi::Value VideoPipelineModule::isRunning(jsi::Runtime&) {
    return jsi::Value(running_.load(std::memory_order_acquire));
}

jsi::Value VideoPipelineModule::getMetrics(jsi::Runtime& rt) {
    jsi::Object result(rt);
    result.setProperty(rt, "avSyncOffsetUs", static_cast<double>(syncCoordinator().currentOffsetUs()));

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
    result.setProperty(rt, "lastDecodeError", static_cast<int>(m.lastDecodeError.load(std::memory_order_relaxed)));
    result.setProperty(rt, "queueDepth", static_cast<int>(frameQueue_.pendingFrames()));
    result.setProperty(rt, "needsKeyFrame", frameQueue_.isAwaitingKeyFrame());
    result.setProperty(rt, "decodeThreadResponsive", decodeThread_.isResponsive());
    result.setProperty(rt, "timeSinceVideoHeartbeatMs", static_cast<double>(decodeThread_.timeSinceLastHeartbeatUs()) / 1000.0);
    return result;
}

}  // namespace videomodule
