#include "MediaPipelineModule.h"
#include "OpusDecoderAdapter.h"
#include "../AudioCoordinator.h"
#include "common/MediaLog.h"
#include "common/JsiUtils.h"
#include "common/ThreadAffinity.h"
#include <cmath>

namespace mediamodule {

namespace jsi = facebook::jsi;

bool MediaPipelineModule::install(jsi::Runtime& rt,
                                   std::shared_ptr<facebook::react::CallInvoker> callInvoker) {
    auto& registry = MediaPipelineRegistry::instance();
    uintptr_t rtId = MediaPipelineRegistry::getRuntimeId(rt);

    if (registry.getModule(rtId)) {
        MEDIA_LOG_W("MediaPipelineModule: already installed for this runtime");
        return true;
    }

    // Initialize thread affinity detection early to avoid latency during audio path
    media::thread_affinity::initializeCoreConfig();

    auto module = std::make_shared<MediaPipelineModule>();
    module->callInvoker_ = callInvoker;
    module->runtime_ = &rt;
    registry.registerModule(rtId, module);

    auto obj = jsi::Object::createFromHostObject(rt, module);
    rt.global().setProperty(rt, "__MediaPipeline", std::move(obj));

    MEDIA_LOG_I("MediaPipelineModule: installed");
    return true;
}

void MediaPipelineModule::uninstall(jsi::Runtime& rt) {
    auto& registry = MediaPipelineRegistry::instance();
    uintptr_t rtId = MediaPipelineRegistry::getRuntimeId(rt);
    
    auto module = registry.getModule(rtId);
    if (!module) return;
    
    // Stop session first, then unregister global callbacks to prevent
    // notification-thread access to module members during teardown.
    // Only then clear local state — no external references remain.
    module->session_.stop();

    media::AudioSessionManager::instance().setRouteCallback(nullptr);
    media::AudioCoordinator::instance().clearPlayback();
    media::AudioCoordinator::instance().setPlaybackRestartFn(nullptr);
    media::AudioCoordinator::instance().setPlaybackPauseFn(nullptr);

    module->evictionCallback_.reset();
    module->routeCallback_.reset();
    module->runtime_ = nullptr;

    rt.global().setProperty(rt, "__MediaPipeline", jsi::Value::undefined());
    registry.unregisterModule(rtId);
    
    MEDIA_LOG_I("MediaPipelineModule: uninstalled");
}

std::shared_ptr<MediaPipelineModule> MediaPipelineModule::getForRuntime(jsi::Runtime& rt) {
    return MediaPipelineRegistry::instance().getModule(MediaPipelineRegistry::getRuntimeId(rt));
}

namespace {

struct BufferView {
    const uint8_t* data = nullptr;
    size_t size = 0;
    
    explicit operator bool() const noexcept { return data && size > 0; }
};

BufferView extractBuffer(jsi::Runtime& rt, const jsi::Value& bufferVal) {
    BufferView result;
    
    if (!bufferVal.isObject()) {
        return result;
    }
    
    jsi::Object obj = bufferVal.getObject(rt);
    
    if (obj.isArrayBuffer(rt)) {
        jsi::ArrayBuffer ab = obj.getArrayBuffer(rt);
        uint8_t* ptr = ab.data(rt);
        size_t sz = ab.size(rt);
        if (ptr && sz > 0) {
            result.data = ptr;
            result.size = sz;
        }
        return result;
    }
    
    jsi::Value bufferProp = obj.getProperty(rt, "buffer");
    if (!bufferProp.isObject()) {
        return result;
    }
    
    jsi::Object bufferObj = bufferProp.getObject(rt);
    if (!bufferObj.isArrayBuffer(rt)) {
        return result;
    }
    
    jsi::ArrayBuffer ab = bufferObj.getArrayBuffer(rt);
    
    uint8_t* basePtr = ab.data(rt);
    size_t totalSize = ab.size(rt);
    if (!basePtr || totalSize == 0) {
        return result;
    }
    
    size_t offset = 0;
    size_t length = totalSize;
    
    jsi::Value offsetVal = obj.getProperty(rt, "byteOffset");
    if (offsetVal.isNumber()) {
        double d = offsetVal.asNumber();
        if (std::isfinite(d) && d >= 0 && d <= static_cast<double>(SIZE_MAX)) {
            offset = static_cast<size_t>(d);
        }
    }
    
    jsi::Value lengthVal = obj.getProperty(rt, "byteLength");
    if (lengthVal.isNumber()) {
        double d = lengthVal.asNumber();
        if (std::isfinite(d) && d > 0 && d <= static_cast<double>(SIZE_MAX)) {
            length = static_cast<size_t>(d);
        }
    }
    
    if (offset > totalSize || length > totalSize - offset) {
        return result;
    }
    
    result.data = basePtr + offset;
    result.size = length;
    
    return result;
}

}  // namespace

enum class MethodId : uint8_t {
    Unknown = 0,
    AddParticipant,
    RemoveParticipant,
    PushAudioFrame,
    SetMuted,
    SetGain,
    GetMetrics,
    Start,
    Stop,
    IsRunning,
    GetLimits,
    ResetWatchdog,
    SetEvictionCallback,
    WarmUp,
    SetAudioRoute,
    GetAvailableAudioRoutes,
    GetAvailableAudioDevices,
    GetCurrentAudioRoute,
    SetAudioRouteCallback
};

namespace {

using jsiutil::operator""_h;

MethodId lookupMethod(const std::string& name) noexcept {
    uint32_t h = jsiutil::hash(name);
    
    switch (h) {
        case "addParticipant"_h:    return name == "addParticipant" ? MethodId::AddParticipant : MethodId::Unknown;
        case "removeParticipant"_h: return name == "removeParticipant" ? MethodId::RemoveParticipant : MethodId::Unknown;
        case "pushAudioFrame"_h:    return name == "pushAudioFrame" ? MethodId::PushAudioFrame : MethodId::Unknown;
        case "setMuted"_h:          return name == "setMuted" ? MethodId::SetMuted : MethodId::Unknown;
        case "setGain"_h:           return name == "setGain" ? MethodId::SetGain : MethodId::Unknown;
        case "getMetrics"_h:        return name == "getMetrics" ? MethodId::GetMetrics : MethodId::Unknown;
        case "start"_h:             return name == "start" ? MethodId::Start : MethodId::Unknown;
        case "stop"_h:              return name == "stop" ? MethodId::Stop : MethodId::Unknown;
        case "isRunning"_h:         return name == "isRunning" ? MethodId::IsRunning : MethodId::Unknown;
        case "getLimits"_h:         return name == "getLimits" ? MethodId::GetLimits : MethodId::Unknown;
        case "resetWatchdog"_h:     return name == "resetWatchdog" ? MethodId::ResetWatchdog : MethodId::Unknown;
        case "setEvictionCallback"_h: return name == "setEvictionCallback" ? MethodId::SetEvictionCallback : MethodId::Unknown;
        case "warmUp"_h:            return name == "warmUp" ? MethodId::WarmUp : MethodId::Unknown;
        case "setAudioRoute"_h:     return name == "setAudioRoute" ? MethodId::SetAudioRoute : MethodId::Unknown;
        case "getAvailableAudioRoutes"_h: return name == "getAvailableAudioRoutes" ? MethodId::GetAvailableAudioRoutes : MethodId::Unknown;
        case "getAvailableAudioDevices"_h: return name == "getAvailableAudioDevices" ? MethodId::GetAvailableAudioDevices : MethodId::Unknown;
        case "getCurrentAudioRoute"_h: return name == "getCurrentAudioRoute" ? MethodId::GetCurrentAudioRoute : MethodId::Unknown;
        case "setAudioRouteCallback"_h: return name == "setAudioRouteCallback" ? MethodId::SetAudioRouteCallback : MethodId::Unknown;
        default: return MethodId::Unknown;
    }
}

}  // namespace

jsi::Value MediaPipelineModule::get(jsi::Runtime& rt, const jsi::PropNameID& name) {
    std::string n = name.utf8(rt);
    
    switch (lookupMethod(n)) {
        case MethodId::AddParticipant:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) {
                    return addParticipant(rt);
                });
                
        case MethodId::RemoveParticipant:
            return jsi::Function::createFromHostFunction(rt, name, 1,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (count < 1 || !args[0].isNumber()) {
                        throw jsi::JSError(rt, "removeParticipant requires participantId (number)");
                    }
                    return removeParticipant(rt, jsiutil::safeIntArg(rt, args[0], "participantId"));
                });
                
        case MethodId::PushAudioFrame:
            return jsi::Function::createFromHostFunction(rt, name, 4,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (count < 4) {
                        throw jsi::JSError(rt, "pushAudioFrame requires (participantId, buffer, ptsUs, durationUs)");
                    }
                    return pushAudioFrame(rt, jsiutil::safeIntArg(rt, args[0], "participantId"), args[1],
                        jsiutil::safeInt64Arg(rt, args[2], "ptsUs"), jsiutil::safeInt64Arg(rt, args[3], "durationUs"));
                });
                
        case MethodId::SetMuted:
            return jsi::Function::createFromHostFunction(rt, name, 2,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (count < 2 || !args[0].isNumber() || !args[1].isBool()) {
                        throw jsi::JSError(rt, "setMuted requires (participantId: number, muted: boolean)");
                    }
                    return setMuted(rt, jsiutil::safeIntArg(rt, args[0], "participantId"), args[1].getBool());
                });
                
        case MethodId::SetGain:
            return jsi::Function::createFromHostFunction(rt, name, 2,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (count < 2 || !args[0].isNumber() || !args[1].isNumber()) {
                        throw jsi::JSError(rt, "setGain requires (participantId: number, gain: number)");
                    }
                    return setGain(rt, jsiutil::safeIntArg(rt, args[0], "participantId"),
                                   jsiutil::safeFloatArg(rt, args[1], "gain"));
                });
                
        case MethodId::GetMetrics:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) {
                    return getMetrics(rt);
                });
                
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
                
        case MethodId::GetLimits:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) { 
                    return getLimits(rt); 
                });
                
        case MethodId::ResetWatchdog:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) {
                    return resetWatchdog(rt);
                });

        case MethodId::SetEvictionCallback:
            return jsi::Function::createFromHostFunction(rt, name, 1,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (count < 1) {
                        throw jsi::JSError(rt, "setEvictionCallback requires a callback function or null");
                    }
                    return setEvictionCallback(rt, args[0]);
                });

        case MethodId::WarmUp:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) {
                    std::lock_guard<std::mutex> lk(lifecycleMtx_);
                    return jsi::Value(session_.warmUp());
                });

        case MethodId::SetAudioRoute:
            return jsi::Function::createFromHostFunction(rt, name, 2,
                [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (count < 1 || !args[0].isNumber()) {
                        throw jsi::JSError(rt, "setAudioRoute requires route (number)");
                    }
                    int route = jsiutil::safeIntArg(rt, args[0], "route");
                    if (route < 1 || route > static_cast<int>(media::AudioRoute::UsbDevice)) {
                        throw jsi::JSError(rt, "setAudioRoute: route out of range");
                    }
                    std::string deviceId;
                    if (count >= 2 && args[1].isString()) {
                        deviceId = args[1].getString(rt).utf8(rt);
                    }
                    bool success = media::AudioSessionManager::instance().setAudioRoute(
                        static_cast<media::AudioRoute>(route), deviceId);
                    return jsi::Value(success);
                });

        case MethodId::GetAvailableAudioRoutes:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) {
                    auto routes = media::AudioSessionManager::instance().getAvailableAudioRoutes();
                    jsi::Array result(rt, routes.size());
                    for (size_t i = 0; i < routes.size(); ++i) {
                        result.setValueAtIndex(rt, i, static_cast<int>(routes[i]));
                    }
                    return result;
                });

        case MethodId::GetAvailableAudioDevices:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) {
                    auto devices = media::AudioSessionManager::instance().getAvailableAudioDevices();
                    jsi::Array result(rt, devices.size());
                    for (size_t i = 0; i < devices.size(); ++i) {
                        jsi::Object obj(rt);
                        obj.setProperty(rt, "route", static_cast<int>(devices[i].route));
                        obj.setProperty(rt, "deviceName",
                            jsi::String::createFromUtf8(rt, devices[i].deviceName));
                        obj.setProperty(rt, "deviceId",
                            jsi::String::createFromUtf8(rt, devices[i].deviceId));
                        result.setValueAtIndex(rt, i, std::move(obj));
                    }
                    return result;
                });

        case MethodId::GetCurrentAudioRoute:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) {
                    media::AudioSessionManager::instance().ensureRouteDetected();
                    auto route = media::AudioSessionManager::instance().currentRoute();
                    return jsi::Value(static_cast<int>(route));
                });

        case MethodId::SetAudioRouteCallback:
            return jsi::Function::createFromHostFunction(rt, name, 1,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (count < 1) {
                        throw jsi::JSError(rt, "setAudioRouteCallback requires a callback function or null");
                    }
                    return setAudioRouteCallback(rt, args[0]);
                });

        case MethodId::Unknown:
        default:
            return jsi::Value::undefined();
    }
}

std::vector<jsi::PropNameID> MediaPipelineModule::getPropertyNames(jsi::Runtime& rt) {
    std::vector<jsi::PropNameID> names;
    names.reserve(18);
    names.push_back(jsi::PropNameID::forAscii(rt, "addParticipant"));
    names.push_back(jsi::PropNameID::forAscii(rt, "removeParticipant"));
    names.push_back(jsi::PropNameID::forAscii(rt, "pushAudioFrame"));
    names.push_back(jsi::PropNameID::forAscii(rt, "setMuted"));
    names.push_back(jsi::PropNameID::forAscii(rt, "setGain"));
    names.push_back(jsi::PropNameID::forAscii(rt, "getMetrics"));
    names.push_back(jsi::PropNameID::forAscii(rt, "start"));
    names.push_back(jsi::PropNameID::forAscii(rt, "stop"));
    names.push_back(jsi::PropNameID::forAscii(rt, "isRunning"));
    names.push_back(jsi::PropNameID::forAscii(rt, "getLimits"));
    names.push_back(jsi::PropNameID::forAscii(rt, "resetWatchdog"));
    names.push_back(jsi::PropNameID::forAscii(rt, "setEvictionCallback"));
    names.push_back(jsi::PropNameID::forAscii(rt, "warmUp"));
    names.push_back(jsi::PropNameID::forAscii(rt, "setAudioRoute"));
    names.push_back(jsi::PropNameID::forAscii(rt, "getAvailableAudioRoutes"));
    names.push_back(jsi::PropNameID::forAscii(rt, "getAvailableAudioDevices"));
    names.push_back(jsi::PropNameID::forAscii(rt, "getCurrentAudioRoute"));
    names.push_back(jsi::PropNameID::forAscii(rt, "setAudioRouteCallback"));
    return names;
}

bool MediaPipelineModule::ensureRunning() {
    if (session_.isRunning()) return true;

    media::AudioCoordinator::instance().initialize();
    media::AudioCoordinator::instance().setPlayback(&session_);
    auto weakSelf = weak_from_this();
    media::AudioCoordinator::instance().setPlaybackRestartFn([weakSelf] {
        auto self = weakSelf.lock();
        if (self && self->session_.isRunning()) {
            self->session_.scheduleAudioRestart();
        }
    });
    media::AudioCoordinator::instance().setPlaybackPauseFn([weakSelf](bool pause) {
        auto self = weakSelf.lock();
        if (self && self->session_.isRunning()) {
            if (pause) {
                self->session_.pauseAudio();
            } else {
                self->session_.resumeAudio();
            }
        }
    });
    
    return session_.start();
}

jsi::Value MediaPipelineModule::addParticipant(jsi::Runtime& rt) {
    std::lock_guard<std::mutex> lk(lifecycleMtx_);
    if (!ensureRunning()) {
        throw jsi::JSError(rt, "addParticipant: failed to start pipeline");
    }

    auto* p = session_.addParticipant();
    if (!p) {
        throw jsi::JSError(rt, "addParticipant: max participants reached");
    }
    return jsi::Value(p->id());
}

jsi::Value MediaPipelineModule::removeParticipant(jsi::Runtime&, int participantId) {
    std::lock_guard<std::mutex> lk(lifecycleMtx_);
    if (participantId < 0) return jsi::Value(false);
    if (!session_.isParticipantActive(participantId)) return jsi::Value(false);
    session_.removeParticipant(participantId);
    return jsi::Value(true);
}

jsi::Value MediaPipelineModule::pushAudioFrame(jsi::Runtime& rt, int participantId,
                                                const jsi::Value& bufferVal,
                                                int64_t ptsUs, int64_t durationUs) {
    // No lifecycle mutex: session_.pushAudioFrame() checks isRunning() atomically,
    // all internal structures are pre-allocated arrays, and JSI calls are single-threaded.
    // Avoids 800 mutex acquisitions/sec at 16 participants.
    BufferView buffer = extractBuffer(rt, bufferVal);
    if (!buffer) return jsi::Value(false);
    return jsi::Value(session_.pushAudioFrame(participantId, buffer.data, buffer.size,
                                               ptsUs, durationUs));
}

jsi::Value MediaPipelineModule::setMuted(jsi::Runtime&, int participantId, bool muted) {
    return jsi::Value(session_.setParticipantMuted(participantId, muted));
}

jsi::Value MediaPipelineModule::setGain(jsi::Runtime&, int participantId, float gain) {
    return jsi::Value(session_.setParticipantGain(participantId, gain));
}

jsi::Value MediaPipelineModule::getMetrics(jsi::Runtime& rt) {
    media::SessionMetrics metrics;
    session_.getMetrics(metrics);
    
    jsi::Object result(rt);
    
    jsi::Object health(rt);
    health.setProperty(rt, "running", session_.isRunning());
    health.setProperty(rt, "responsive", metrics.decodeThreadResponsive);
    health.setProperty(rt, "detached", metrics.decodeThreadDetached);
    health.setProperty(rt, "watchdogTripped", metrics.watchdogTripped);
    health.setProperty(rt, "watchdogTripCount", static_cast<int>(metrics.watchdogTripCount));
    health.setProperty(rt, "timeSinceHeartbeatMs", static_cast<double>(metrics.timeSinceHeartbeatUs / 1000));
    result.setProperty(rt, "health", std::move(health));
    
    uint64_t totalUnderruns = 0;
    uint64_t totalDropped = 0;
    uint64_t totalDrained = 0;
    uint64_t totalDecodeErrors = 0;
    uint64_t totalDecoderResets = 0;
    uint64_t totalDiscontinuities = 0;
    uint64_t totalFastPathSwitches = 0;
    uint64_t totalDeadZoneSnaps = 0;
    int64_t maxInterFrameGapUs = 0;
    uint64_t totalGapsOver50ms = 0;
    uint64_t totalGapsOver100ms = 0;
    uint64_t totalGapsOver500ms = 0;

    for (size_t i = 0; i < static_cast<size_t>(media::config::limits::kMaxParticipants); ++i) {
        const auto& p = metrics.participants[i];
        if (p.active && p.audioSlot >= 0) {
            totalUnderruns += p.underruns;
            totalDropped += p.framesDropped;
            totalDrained += p.framesDrained;
            totalDecodeErrors += p.decodeErrors;
            totalDecoderResets += p.decoderResets;
            totalDiscontinuities += p.ptsDiscontinuities;
            totalFastPathSwitches += p.fastPathSwitches;
            totalDeadZoneSnaps += p.catchupDeadZoneSnaps;
            if (p.maxInterFrameGapUs > maxInterFrameGapUs) {
                maxInterFrameGapUs = p.maxInterFrameGapUs;
            }
            totalGapsOver50ms += p.gapsOver50ms;
            totalGapsOver100ms += p.gapsOver100ms;
            totalGapsOver500ms += p.gapsOver500ms;
        }
    }

    jsi::Object quality(rt);
    quality.setProperty(rt, "underruns", static_cast<double>(totalUnderruns));
    quality.setProperty(rt, "framesDropped", static_cast<double>(totalDropped));
    quality.setProperty(rt, "framesDrained", static_cast<double>(totalDrained));
    quality.setProperty(rt, "decodeErrors", static_cast<double>(totalDecodeErrors));
    quality.setProperty(rt, "decoderResets", static_cast<double>(totalDecoderResets));
    quality.setProperty(rt, "ptsDiscontinuities", static_cast<double>(totalDiscontinuities));
    quality.setProperty(rt, "fastPathSwitches", static_cast<double>(totalFastPathSwitches));
    quality.setProperty(rt, "catchupDeadZoneSnaps", static_cast<double>(totalDeadZoneSnaps));
    quality.setProperty(rt, "softClipClamps", static_cast<double>(metrics.softClipClamps));
    quality.setProperty(rt, "maxInterFrameGapMs", static_cast<double>(maxInterFrameGapUs) / 1000.0);
    quality.setProperty(rt, "gapsOver50ms", static_cast<double>(totalGapsOver50ms));
    quality.setProperty(rt, "gapsOver100ms", static_cast<double>(totalGapsOver100ms));
    quality.setProperty(rt, "gapsOver500ms", static_cast<double>(totalGapsOver500ms));
    result.setProperty(rt, "quality", std::move(quality));
    
    jsi::Object session(rt);
    session.setProperty(rt, "uptimeMs", static_cast<double>(metrics.uptimeUs / 1000));
    session.setProperty(rt, "activeParticipants", metrics.activeParticipants);
    session.setProperty(rt, "boundAudioSlots", metrics.boundAudioSlots);
    session.setProperty(rt, "samplesOutput", static_cast<double>(metrics.totalSamplesOutput));
    result.setProperty(rt, "session", std::move(session));
    
    // Latency mode reporting
    jsi::Object latency(rt);
    latency.setProperty(rt, "mode", media::latencyModeToString(metrics.grantedLatencyMode));
    latency.setProperty(rt, "isLowestLatency", metrics.grantedLatencyMode == media::LatencyMode::LowLatency);
    result.setProperty(rt, "latency", std::move(latency));
    
    // New: Pool pressure reporting for backpressure signaling
    jsi::Object pools(rt);
    pools.setProperty(rt, "decodedUnderPressure", metrics.decodedPoolUnderPressure);
    pools.setProperty(rt, "encodedUnderPressure", metrics.encodedPoolUnderPressure);
    result.setProperty(rt, "pools", std::move(pools));

    // Audio output health metrics
    jsi::Object audioOutput(rt);
    audioOutput.setProperty(rt, "restartCount", static_cast<int>(metrics.audioRestartCount));
    audioOutput.setProperty(rt, "lastError", metrics.audioLastError);
    audioOutput.setProperty(rt, "actualSampleRate", metrics.actualSampleRate);
    audioOutput.setProperty(rt, "latencyUs", static_cast<double>(metrics.audioOutputLatencyUs));
    result.setProperty(rt, "audioOutput", std::move(audioOutput));

    // Bluetooth route diagnostics
    jsi::Object bluetooth(rt);
    bluetooth.setProperty(rt, "route", media::audioRouteToString(metrics.currentRoute));
    bluetooth.setProperty(rt, "isA2dp", metrics.currentRoute == media::AudioRoute::BluetoothA2dp);
    result.setProperty(rt, "bluetooth", std::move(bluetooth));

    int activeCount = 0;
    for (size_t i = 0; i < static_cast<size_t>(media::config::limits::kMaxParticipants); ++i) {
        if (metrics.participants[i].active) activeCount++;
    }

    jsi::Array participants(rt, static_cast<size_t>(activeCount));
    size_t idx = 0;
    for (size_t i = 0; i < static_cast<size_t>(media::config::limits::kMaxParticipants) &&
         idx < static_cast<size_t>(activeCount); ++i) {
        const auto& p = metrics.participants[i];
        if (!p.active) continue;

        jsi::Object participant(rt);
        participant.setProperty(rt, "id", static_cast<int>(i));
        participant.setProperty(rt, "audioSlot", p.audioSlot);
        participant.setProperty(rt, "framesReceived", static_cast<double>(p.audioFramesReceived));
        participant.setProperty(rt, "underruns", static_cast<double>(p.underruns));
        participant.setProperty(rt, "muted", p.muted);
        participant.setProperty(rt, "jitterUs", static_cast<double>(p.jitterUs));
        participant.setProperty(rt, "bufferTargetUs", static_cast<double>(p.bufferTargetUs));
        participant.setProperty(rt, "networkStability", static_cast<double>(p.networkStability));

        participants.setValueAtIndex(rt, idx++, std::move(participant));
    }
    result.setProperty(rt, "participants", std::move(participants));
    
    return result;
}

jsi::Value MediaPipelineModule::start(jsi::Runtime&) {
    std::lock_guard<std::mutex> lk(lifecycleMtx_);
    return jsi::Value(ensureRunning());
}

jsi::Value MediaPipelineModule::stop(jsi::Runtime&) {
    std::lock_guard<std::mutex> lk(lifecycleMtx_);
    
    media::AudioCoordinator::instance().clearPlayback();
    media::AudioCoordinator::instance().setPlaybackRestartFn(nullptr);
    media::AudioCoordinator::instance().setPlaybackPauseFn(nullptr);
    
    session_.stop();
    return jsi::Value(true);
}

jsi::Value MediaPipelineModule::isRunning(jsi::Runtime&) {
    return jsi::Value(session_.isRunning());
}

jsi::Value MediaPipelineModule::getLimits(jsi::Runtime& rt) {
    jsi::Object result(rt);
    result.setProperty(rt, "maxParticipants", session_.maxParticipants());
    result.setProperty(rt, "maxAudioSlots", session_.maxAudioSlots());
    result.setProperty(rt, "nativeVersion", media::config::kVersion);
    return result;
}

jsi::Value MediaPipelineModule::resetWatchdog(jsi::Runtime&) {
    session_.resetWatchdogState();
    return jsi::Value(true);
}

jsi::Value MediaPipelineModule::setEvictionCallback(jsi::Runtime& rt, const jsi::Value& callback) {
    if (callback.isNull() || callback.isUndefined()) {
        evictionCallback_.reset();
        session_.setEvictionCallback(nullptr);
        MEDIA_LOG_I("MediaPipelineModule: eviction callback cleared");
        return jsi::Value(true);
    }

    if (!callback.isObject() || !callback.asObject(rt).isFunction(rt)) {
        throw jsi::JSError(rt, "setEvictionCallback requires a function or null");
    }

    evictionCallback_ = std::make_shared<jsi::Function>(callback.asObject(rt).asFunction(rt));
    setupEvictionCallback(rt);
    MEDIA_LOG_I("MediaPipelineModule: eviction callback set");
    return jsi::Value(true);
}

void MediaPipelineModule::setupEvictionCallback(jsi::Runtime& rt) {
    if (!evictionCallback_ || !callInvoker_) {
        MEDIA_LOG_W("MediaPipelineModule: cannot setup eviction callback - missing invoker or callback");
        return;
    }

    auto weakSelf = weak_from_this();
    session_.setEvictionCallback([weakSelf](int evictedId, int replacedById, media::EvictionReason reason) {
        auto self = weakSelf.lock();
        if (!self || !self->evictionCallback_ || !self->callInvoker_ || !self->runtime_) {
            return;
        }

        int reasonInt = static_cast<int>(reason);

        self->callInvoker_->invokeAsync([weakSelf, evictedId, replacedById, reasonInt]() {
            auto self = weakSelf.lock();
            if (!self || !self->runtime_ || !self->evictionCallback_) return;
            try {
                auto& rt = *self->runtime_;
                jsi::Object event(rt);
                event.setProperty(rt, "evictedParticipantId", evictedId);
                event.setProperty(rt, "replacedByParticipantId", replacedById);
                event.setProperty(rt, "reason", reasonInt);
                // 0 = LRUSlotExhaustion, 1 = InactivityTimeout
                event.setProperty(rt, "reasonName",
                    reasonInt == 0 ? "slot_exhaustion" : "inactivity_timeout");

                self->evictionCallback_->call(rt, event);
            } catch (const std::exception& e) {
                MEDIA_LOG_E("MediaPipelineModule: eviction callback error: %s", e.what());
            }
        });
    });
}

jsi::Value MediaPipelineModule::setAudioRouteCallback(jsi::Runtime& rt, const jsi::Value& callback) {
    if (callback.isNull() || callback.isUndefined()) {
        media::AudioSessionManager::instance().setRouteCallback(nullptr);
        routeCallback_.reset();
        MEDIA_LOG_I("MediaPipelineModule: route callback cleared");
        return jsi::Value(true);
    }

    if (!callback.isObject() || !callback.asObject(rt).isFunction(rt)) {
        throw jsi::JSError(rt, "setAudioRouteCallback requires a function or null");
    }

    routeCallback_ = std::make_shared<jsi::Function>(callback.asObject(rt).asFunction(rt));
    setupAudioRouteCallback(rt);

    // Fire initial route event so JS gets current state immediately.
    // ensureRouteDetected() queries the OS if route is still Unknown (pre-init).
    // Skip if still Unknown — the first route change notification will deliver it.
    auto& mgr = media::AudioSessionManager::instance();
    mgr.ensureRouteDetected();
    media::AudioRoute currentRoute = mgr.currentRoute();

    if (currentRoute != media::AudioRoute::Unknown) {
        media::EchoMode currentEcho = mgr.currentEchoMode();
        auto availableDevices = mgr.getAvailableAudioDevices();

        jsi::Object event(rt);
        event.setProperty(rt, "route", static_cast<int>(currentRoute));
        event.setProperty(rt, "echoMode", static_cast<int>(currentEcho));

        jsi::Array devicesArray(rt, availableDevices.size());
        for (size_t i = 0; i < availableDevices.size(); ++i) {
            jsi::Object obj(rt);
            obj.setProperty(rt, "route", static_cast<int>(availableDevices[i].route));
            obj.setProperty(rt, "deviceName",
                jsi::String::createFromUtf8(rt, availableDevices[i].deviceName));
            obj.setProperty(rt, "deviceId",
                jsi::String::createFromUtf8(rt, availableDevices[i].deviceId));
            devicesArray.setValueAtIndex(rt, i, std::move(obj));
        }
        event.setProperty(rt, "availableDevices", std::move(devicesArray));

        routeCallback_->call(rt, event);
    }

    MEDIA_LOG_I("MediaPipelineModule: route callback set (initial route=%s)",
                media::audioRouteToString(currentRoute));
    return jsi::Value(true);
}

void MediaPipelineModule::setupAudioRouteCallback(jsi::Runtime&) {
    if (!routeCallback_ || !callInvoker_) {
        MEDIA_LOG_W("MediaPipelineModule: cannot setup route callback - missing invoker or callback");
        return;
    }

    // Wire drift reset through RouteHandler so it triggers on actual route changes.
    auto weakSelf = weak_from_this();
    auto& mgr = media::AudioSessionManager::instance();
    mgr.setDriftResetCallback([weakSelf]() {
        auto self = weakSelf.lock();
        if (self && self->session_.isRunning()) {
            self->session_.requestDriftResetAll();
        }
    });

    // Route callback: RouteHandler fires this via fireJsCallback.
    // Dedup is handled by RouteHandler. We forward to JS via CallInvoker.
    mgr.setRouteCallback(
        [weakSelf](media::AudioRoute route, media::EchoMode echoMode) {
            auto self = weakSelf.lock();
            if (!self || !self->routeCallback_ || !self->callInvoker_ || !self->runtime_) {
                return;
            }

            int routeInt = static_cast<int>(route);
            int echoModeInt = static_cast<int>(echoMode);
            auto availableDevices = media::AudioSessionManager::instance().getAvailableAudioDevices();

            self->callInvoker_->invokeAsync([weakSelf, routeInt, echoModeInt, availableDevices]() {
                auto self = weakSelf.lock();
                if (!self || !self->runtime_ || !self->routeCallback_) return;
                try {
                    auto& rt = *self->runtime_;
                    jsi::Object event(rt);
                    event.setProperty(rt, "route", routeInt);
                    event.setProperty(rt, "echoMode", echoModeInt);

                    jsi::Array devicesArray(rt, availableDevices.size());
                    for (size_t i = 0; i < availableDevices.size(); ++i) {
                        jsi::Object obj(rt);
                        obj.setProperty(rt, "route", static_cast<int>(availableDevices[i].route));
                        obj.setProperty(rt, "deviceName",
                            jsi::String::createFromUtf8(rt, availableDevices[i].deviceName));
                        obj.setProperty(rt, "deviceId",
                            jsi::String::createFromUtf8(rt, availableDevices[i].deviceId));
                        devicesArray.setValueAtIndex(rt, i, std::move(obj));
                    }
                    event.setProperty(rt, "availableDevices", std::move(devicesArray));

                    self->routeCallback_->call(rt, event);
                } catch (const std::exception& e) {
                    MEDIA_LOG_E("MediaPipelineModule: route callback error: %s", e.what());
                }
            });
        });
}

}  // namespace mediamodule
