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
    module->callInvoker_ = callInvoker;
    module->runtime_ = &rt;
    registry.registerModule(rtId, module);

    auto obj = jsi::Object::createFromHostObject(rt, module);
    rt.global().setProperty(rt, "__VideoPipeline", std::move(obj));

    MEDIA_LOG_I("VideoPipelineModule: installed");
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

    rt.global().setProperty(rt, "__VideoPipeline", jsi::Value::undefined());
    registry.unregisterModule(rtId);

    MEDIA_LOG_I("VideoPipelineModule: uninstalled");
}

VideoPipelineModule::~VideoPipelineModule() { stopDecodeThread(); }

namespace {

struct BufferView {
    const uint8_t* data = nullptr;
    size_t size = 0;
    explicit operator bool() const noexcept { return data && size > 0; }
};

BufferView extractBuffer(jsi::Runtime& rt, const jsi::Value& bufferVal) {
    BufferView result;
    if (!bufferVal.isObject()) return result;

    jsi::Object obj = bufferVal.getObject(rt);
    if (obj.isArrayBuffer(rt)) {
        jsi::ArrayBuffer ab = obj.getArrayBuffer(rt);
        uint8_t* ptr = ab.data(rt);
        size_t sz = ab.size(rt);
        if (ptr && sz > 0) { result.data = ptr; result.size = sz; }
        return result;
    }

    jsi::Value bufferProp = obj.getProperty(rt, "buffer");
    if (!bufferProp.isObject()) return result;
    jsi::Object bufferObj = bufferProp.getObject(rt);
    if (!bufferObj.isArrayBuffer(rt)) return result;

    jsi::ArrayBuffer ab = bufferObj.getArrayBuffer(rt);
    uint8_t* basePtr = ab.data(rt);
    size_t totalSize = ab.size(rt);
    if (!basePtr || totalSize == 0) return result;

    size_t offset = 0, length = totalSize;
    jsi::Value offsetVal = obj.getProperty(rt, "byteOffset");
    if (offsetVal.isNumber()) {
        double d = offsetVal.asNumber();
        if (std::isfinite(d) && d >= 0) offset = static_cast<size_t>(d);
    }
    jsi::Value lengthVal = obj.getProperty(rt, "byteLength");
    if (lengthVal.isNumber()) {
        double d = lengthVal.asNumber();
        if (std::isfinite(d) && d > 0) length = static_cast<size_t>(d);
    }
    if (offset > totalSize || length > totalSize - offset) return result;
    result.data = basePtr + offset;
    result.size = length;
    return result;
}

enum class MethodId : uint8_t {
    Unknown = 0, Start, Stop, IsRunning, PushVideoFrame,
    RemoveParticipant, RequestKeyFrame, GetMetrics
};

using jsiutil::operator""_h;

MethodId lookupMethod(const std::string& name) noexcept {
    switch (jsiutil::hash(name)) {
        case "start"_h:             return name == "start" ? MethodId::Start : MethodId::Unknown;
        case "stop"_h:              return name == "stop" ? MethodId::Stop : MethodId::Unknown;
        case "isRunning"_h:         return name == "isRunning" ? MethodId::IsRunning : MethodId::Unknown;
        case "pushVideoFrame"_h:    return name == "pushVideoFrame" ? MethodId::PushVideoFrame : MethodId::Unknown;
        case "removeParticipant"_h: return name == "removeParticipant" ? MethodId::RemoveParticipant : MethodId::Unknown;
        case "requestKeyFrame"_h:   return name == "requestKeyFrame" ? MethodId::RequestKeyFrame : MethodId::Unknown;
        case "getMetrics"_h:        return name == "getMetrics" ? MethodId::GetMetrics : MethodId::Unknown;
        default: return MethodId::Unknown;
    }
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
        case MethodId::PushVideoFrame:
            return jsi::Function::createFromHostFunction(rt, name, 4,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (count < 4) throw jsi::JSError(rt, "pushVideoFrame requires (participantId, buffer, ptsUs, isKeyFrame)");
                    return pushVideoFrame(rt,
                        jsiutil::safeIntArg(rt, args[0], "participantId"), args[1],
                        jsiutil::safeInt64Arg(rt, args[2], "ptsUs"),
                        args[3].isBool() ? args[3].getBool() : false);
                });
        case MethodId::RemoveParticipant:
            return jsi::Function::createFromHostFunction(rt, name, 1,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (count < 1 || !args[0].isNumber()) throw jsi::JSError(rt, "removeParticipant requires participantId");
                    return removeParticipant(rt, jsiutil::safeIntArg(rt, args[0], "participantId"));
                });
        case MethodId::RequestKeyFrame:
            return jsi::Function::createFromHostFunction(rt, name, 1,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (count < 1 || !args[0].isNumber()) throw jsi::JSError(rt, "requestKeyFrame requires participantId");
                    return requestKeyFrame(rt, jsiutil::safeIntArg(rt, args[0], "participantId"));
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
    names.reserve(7);
    names.push_back(jsi::PropNameID::forAscii(rt, "start"));
    names.push_back(jsi::PropNameID::forAscii(rt, "stop"));
    names.push_back(jsi::PropNameID::forAscii(rt, "isRunning"));
    names.push_back(jsi::PropNameID::forAscii(rt, "pushVideoFrame"));
    names.push_back(jsi::PropNameID::forAscii(rt, "removeParticipant"));
    names.push_back(jsi::PropNameID::forAscii(rt, "requestKeyFrame"));
    names.push_back(jsi::PropNameID::forAscii(rt, "getMetrics"));
    return names;
}

int VideoPipelineModule::getOrCreateSlot(int participantId) {
    std::lock_guard<std::mutex> lk(slotMapMtx_);
    auto it = participantToSlot_.find(participantId);
    if (it != participantToSlot_.end()) return it->second;
    for (int i = 0; i < media::video_config::kMaxVideoSlots; ++i) {
        if (!slots_[i].isBound()) {
            slots_[i].bind(participantId);
            participantToSlot_[participantId] = i;
            return i;
        }
    }
    return -1;
}

void VideoPipelineModule::releaseSlot(int participantId) {
    int slot;
    {
        std::lock_guard<std::mutex> lk(slotMapMtx_);
        auto it = participantToSlot_.find(participantId);
        if (it == participantToSlot_.end()) return;
        slot = it->second;
        slots_[slot].unbind();
        renderClocks_[slot].reset();
        fpsCounters_[slot].reset();
        lastAnchorTimeUs_[slot] = 0;
        participantToSlot_.erase(it);
    }
    std::lock_guard<std::mutex> lk(decoderMtx_);
    decoders_[slot].reset();
}

jsi::Value VideoPipelineModule::start(jsi::Runtime&) {
    std::lock_guard<std::mutex> lk(lifecycleMtx_);
    if (running_.load(std::memory_order_acquire)) return jsi::Value(true);
    running_.store(true, std::memory_order_release);
    startDecodeThread();
    MEDIA_LOG_I("VideoPipelineModule: started");
    return jsi::Value(true);
}

jsi::Value VideoPipelineModule::stop(jsi::Runtime&) {
    std::lock_guard<std::mutex> lk(lifecycleMtx_);
    if (!running_.load(std::memory_order_acquire)) return jsi::Value(true);
    running_.store(false, std::memory_order_release);
    stopDecodeThread();
    {
        std::lock_guard<std::mutex> slk(slotMapMtx_);
        for (auto& [pid, slot] : participantToSlot_) {
            slots_[slot].unbind();
            renderClocks_[slot].reset();
        }
        participantToSlot_.clear();
    }
    lastAnchorTimeUs_.fill(0);
    syncCoordinator_.reset();
    MEDIA_LOG_I("VideoPipelineModule: stopped");
    return jsi::Value(true);
}

jsi::Value VideoPipelineModule::isRunning(jsi::Runtime&) {
    return jsi::Value(running_.load(std::memory_order_acquire));
}

jsi::Value VideoPipelineModule::pushVideoFrame(jsi::Runtime& rt, int participantId,
                                                const jsi::Value& bufferVal,
                                                int64_t ptsUs, bool isKeyFrame) {
    if (!running_.load(std::memory_order_acquire)) return jsi::Value(false);
    BufferView buffer = extractBuffer(rt, bufferVal);
    if (!buffer) return jsi::Value(false);
    int slot = getOrCreateSlot(participantId);
    if (slot < 0) return jsi::Value(false);
    return jsi::Value(slots_[slot].pushEncodedFrame(buffer.data, buffer.size, ptsUs, isKeyFrame));
}

jsi::Value VideoPipelineModule::removeParticipant(jsi::Runtime&, int participantId) {
    if (participantId < 0) return jsi::Value(false);
    releaseSlot(participantId);
    return jsi::Value(true);
}

jsi::Value VideoPipelineModule::requestKeyFrame(jsi::Runtime&, int participantId) {
    std::lock_guard<std::mutex> lk(slotMapMtx_);
    auto it = participantToSlot_.find(participantId);
    if (it == participantToSlot_.end()) return jsi::Value(false);
    slots_[it->second].requestKeyFrame();
    return jsi::Value(true);
}

jsi::Value VideoPipelineModule::getMetrics(jsi::Runtime& rt) {
    jsi::Object result(rt);
    std::lock_guard<std::mutex> lk(slotMapMtx_);
    int activeCount = static_cast<int>(participantToSlot_.size());
    result.setProperty(rt, "activeStreams", activeCount);
    result.setProperty(rt, "avSyncOffsetUs", static_cast<double>(syncCoordinator_.currentOffsetUs()));

    jsi::Array participants(rt, static_cast<size_t>(activeCount));
    size_t idx = 0;
    for (auto& [pid, slotIdx] : participantToSlot_) {
        const auto& m = slots_[slotIdx].metrics();
        jsi::Object p(rt);
        p.setProperty(rt, "id", pid);
        p.setProperty(rt, "framesReceived", static_cast<double>(m.framesReceived.load(std::memory_order_relaxed)));
        p.setProperty(rt, "framesDecoded", static_cast<double>(m.framesDecoded.load(std::memory_order_relaxed)));
        p.setProperty(rt, "framesDropped", static_cast<double>(m.framesDropped.load(std::memory_order_relaxed)));
        p.setProperty(rt, "decodeErrors", static_cast<double>(m.decodeErrors.load(std::memory_order_relaxed)));
        p.setProperty(rt, "currentFps", m.currentFps.load(std::memory_order_relaxed));
        p.setProperty(rt, "width", m.width.load(std::memory_order_relaxed));
        p.setProperty(rt, "height", m.height.load(std::memory_order_relaxed));
        p.setProperty(rt, "jitterUs", static_cast<double>(m.jitterUs.load(std::memory_order_relaxed)));
        p.setProperty(rt, "bufferTargetUs", static_cast<double>(m.bufferTargetUs.load(std::memory_order_relaxed)));
        p.setProperty(rt, "lateFrames", static_cast<double>(m.lateFrames.load(std::memory_order_relaxed)));
        p.setProperty(rt, "skippedFrames", static_cast<double>(m.skippedFrames.load(std::memory_order_relaxed)));
        participants.setValueAtIndex(rt, idx++, std::move(p));
    }
    result.setProperty(rt, "participants", std::move(participants));
    return result;
}

void VideoPipelineModule::startDecodeThread() {
    if (decodeRunning_.load(std::memory_order_acquire)) return;
    decodeRunning_.store(true, std::memory_order_release);
    decodeThread_ = std::thread(&VideoPipelineModule::decodeLoop, this);
}

void VideoPipelineModule::stopDecodeThread() {
    decodeRunning_.store(false, std::memory_order_release);
    if (decodeThread_.joinable()) {
        decodeThread_.join();
    }
    std::lock_guard<std::mutex> lk(decoderMtx_);
    for (auto& decoder : decoders_) {
        decoder.reset();
    }
    for (auto& fps : fpsCounters_) {
        fps.reset();
    }
}

void VideoPipelineModule::decodeLoop() {
    pthread_setname_np("VideoDecodeThread");

    media::EncodedVideoFrame frame;

    while (decodeRunning_.load(std::memory_order_acquire)) {
        @autoreleasepool {
            bool didWork = false;

            for (int i = 0; i < media::video_config::kMaxVideoSlots; ++i) {
                auto& slot = slots_[i];
                if (!slot.isBound()) {
                    std::lock_guard<std::mutex> lk(decoderMtx_);
                    if (decoders_[i]) decoders_[i].reset();
                    continue;
                }

                int participantId = slot.boundParticipant();

                if (!media::VideoSurfaceRegistry::instance().hasSurface(participantId)) {
                    std::lock_guard<std::mutex> lk(decoderMtx_);
                    if (decoders_[i]) decoders_[i].reset();
                    continue;
                }

                auto& clock = renderClocks_[i];
                int64_t now = media::nowUs();

                // Anchor render clock on first frame
                if (!clock.isAnchored()) {
                    int64_t nextPts;
                    if (slot.peekNextPts(nextPts)) {
                        int64_t adjustedTarget = slot.bufferTargetUs() + syncCoordinator_.videoBufferAdjustmentUs();
                        adjustedTarget = std::clamp(adjustedTarget,
                                                    media::video_config::jitter::kMinBufferUs,
                                                    media::video_config::jitter::kMaxBufferUs);
                        clock.anchor(nextPts, now, adjustedTarget);
                        lastAnchorTimeUs_[i] = now;
                    }
                } else if (lastAnchorTimeUs_[i] > 0 &&
                           (now - lastAnchorTimeUs_[i]) >= media::video_config::kReanchorIntervalUs) {
                    // Periodic re-anchor to compensate for clock drift
                    clock.reset();
                    lastAnchorTimeUs_[i] = 0;
                }

                // Update buffer target with A/V sync correction
                int64_t adjustedTarget = slot.bufferTargetUs() + syncCoordinator_.videoBufferAdjustmentUs();
                adjustedTarget = std::clamp(adjustedTarget,
                                            media::video_config::jitter::kMinBufferUs,
                                            media::video_config::jitter::kMaxBufferUs);
                clock.updateBufferTarget(adjustedTarget);

                // Update per-slot jitter metrics
                slot.metrics().jitterUs.store(slot.jitterUs(), std::memory_order_relaxed);
                slot.metrics().bufferTargetUs.store(slot.bufferTargetUs(), std::memory_order_relaxed);

                // Single-lock pop with readiness check and late-frame skip
                auto popResult = slot.tryPopReadyFrame(frame, clock, now);
                if (popResult != media::PopResult::Nothing) {
                    VP9Decoder* decoder = nullptr;
                    {
                        std::lock_guard<std::mutex> lk(decoderMtx_);
                        if (!decoders_[i]) {
                            auto surface = media::VideoSurfaceRegistry::instance().acquireSurface(participantId);
                            if (surface) {
                                auto d = std::make_unique<VP9Decoder>();
                                d->setOutputLayer((__bridge AVSampleBufferDisplayLayer*)surface);
                                CFRelease(surface);
                                int pid = participantId;
                                d->setKeyFrameRequestFn([pid]() {
                                    media::VideoSurfaceRegistry::instance().requestKeyFrame(pid);
                                });
                                decoders_[i] = std::move(d);
                            }
                        }
                        if (decoders_[i]) decoder = decoders_[i].get();
                    }

                    if (decoder) {
                        if (decoder->submitFrame(frame.data.data(), frame.data.size(),
                                                  frame.ptsUs, frame.isKeyFrame)) {
                            slot.metrics().framesDecoded.fetch_add(1, std::memory_order_relaxed);
                            slot.metrics().currentFps.store(
                                fpsCounters_[i].tick(media::nowUs()), std::memory_order_relaxed);
                            if (decoder->decodedWidth() > 0) {
                                slot.metrics().width.store(decoder->decodedWidth(), std::memory_order_relaxed);
                                slot.metrics().height.store(decoder->decodedHeight(), std::memory_order_relaxed);
                                media::VideoSurfaceRegistry::instance().setDecodedResolution(
                                    participantId, decoder->decodedWidth(), decoder->decodedHeight());
                            }
                            syncCoordinator_.onVideoRender(frame.ptsUs, media::nowUs());
                        } else {
                            slot.metrics().decodeErrors.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    didWork = true;
                }
            }

            if (!didWork) {
                std::this_thread::sleep_for(std::chrono::microseconds(2000));
            }
        }
    }
}

}  // namespace videomodule
