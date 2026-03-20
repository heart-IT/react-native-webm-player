#include "MediaPipelineModule.h"
#include "OpusDecoderAdapter.h"
#include "common/MediaLog.h"
#include "common/JsiUtils.h"
#include "common/ThreadAffinity.h"
#include "video/VideoSurfaceRegistry.h"
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

    module->routeCallback_.reset();
    module->healthCallback_.reset();
    module->keyFrameNeededCallback_.reset();
    module->audioFocusCallback_.reset();
    media::VideoSurfaceRegistry::instance().setKeyFrameRequestFn(nullptr);
    module->runtime_ = nullptr;

    rt.global().setProperty(rt, "__MediaPipeline", jsi::Value::undefined());
    registry.unregisterModule(rtId);

    MEDIA_LOG_I("MediaPipelineModule: uninstalled");
}

std::shared_ptr<MediaPipelineModule> MediaPipelineModule::getForRuntime(jsi::Runtime& rt) {
    return MediaPipelineRegistry::instance().getModule(MediaPipelineRegistry::getRuntimeId(rt));
}

namespace {

using jsiutil::BufferView;
using jsiutil::extractBuffer;

}  // namespace

enum class MethodId : uint8_t {
    Unknown = 0,
    FeedData,
    SetMuted,
    SetGain,
    GetMetrics,
    Start,
    Stop,
    IsRunning,
    SetAudioRoute,
    GetAvailableAudioRoutes,
    GetAvailableAudioDevices,
    GetCurrentAudioRoute,
    SetAudioRouteCallback,
    SetHealthCallback,
    GetTrackInfo,
    ResetStream,
    RequestKeyFrame,
    WarmUp,
    SetKeyFrameNeededCallback,
    Pause,
    Resume,
    IsPaused,
    GetCurrentTimeUs,
    GetPlaybackState,
    SetBufferTarget,
    SetAudioFocusCallback,
    SetCatchupPolicy,
    SetStreamStatus,
    SetPlaybackRate,
    SetClipBufferDuration,
    CaptureClip
};

namespace {

MethodId lookupMethod(const std::string& name) noexcept {
    if (name == "feedData") return MethodId::FeedData;
    if (name == "setMuted") return MethodId::SetMuted;
    if (name == "setGain") return MethodId::SetGain;
    if (name == "getMetrics") return MethodId::GetMetrics;
    if (name == "start") return MethodId::Start;
    if (name == "stop") return MethodId::Stop;
    if (name == "isRunning") return MethodId::IsRunning;
    if (name == "setAudioRoute") return MethodId::SetAudioRoute;
    if (name == "getAvailableAudioRoutes") return MethodId::GetAvailableAudioRoutes;
    if (name == "getAvailableAudioDevices") return MethodId::GetAvailableAudioDevices;
    if (name == "getCurrentAudioRoute") return MethodId::GetCurrentAudioRoute;
    if (name == "setAudioRouteCallback") return MethodId::SetAudioRouteCallback;
    if (name == "setHealthCallback") return MethodId::SetHealthCallback;
    if (name == "getTrackInfo") return MethodId::GetTrackInfo;
    if (name == "resetStream") return MethodId::ResetStream;
    if (name == "requestKeyFrame") return MethodId::RequestKeyFrame;
    if (name == "warmUp") return MethodId::WarmUp;
    if (name == "setKeyFrameNeededCallback") return MethodId::SetKeyFrameNeededCallback;
    if (name == "pause") return MethodId::Pause;
    if (name == "resume") return MethodId::Resume;
    if (name == "isPaused") return MethodId::IsPaused;
    if (name == "getCurrentTimeUs") return MethodId::GetCurrentTimeUs;
    if (name == "getPlaybackState") return MethodId::GetPlaybackState;
    if (name == "setBufferTarget") return MethodId::SetBufferTarget;
    if (name == "setAudioFocusCallback") return MethodId::SetAudioFocusCallback;
    if (name == "setCatchupPolicy") return MethodId::SetCatchupPolicy;
    if (name == "setStreamStatus") return MethodId::SetStreamStatus;
    if (name == "setPlaybackRate") return MethodId::SetPlaybackRate;
    if (name == "setClipBufferDuration") return MethodId::SetClipBufferDuration;
    if (name == "captureClip") return MethodId::CaptureClip;
    return MethodId::Unknown;
}

}  // namespace

jsi::Value MediaPipelineModule::get(jsi::Runtime& rt, const jsi::PropNameID& name) {
    std::string n = name.utf8(rt);

    switch (lookupMethod(n)) {
        case MethodId::FeedData:
            return jsi::Function::createFromHostFunction(rt, name, 1,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (count < 1) {
                        throw jsi::JSError(rt, "feedData requires (buffer)");
                    }
                    return feedData(rt, args[0]);
                });

        case MethodId::SetMuted:
            return jsi::Function::createFromHostFunction(rt, name, 1,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (count < 1 || !args[0].isBool()) {
                        throw jsi::JSError(rt, "setMuted requires (muted: boolean)");
                    }
                    return setMuted(rt, args[0].getBool());
                });

        case MethodId::SetGain:
            return jsi::Function::createFromHostFunction(rt, name, 1,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (count < 1 || !args[0].isNumber()) {
                        throw jsi::JSError(rt, "setGain requires (gain: number)");
                    }
                    return setGain(rt, jsiutil::safeFloatArg(rt, args[0], "gain"));
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

        case MethodId::SetAudioRoute:
            return jsi::Function::createFromHostFunction(rt, name, 2,
                [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (count < 1 || !args[0].isNumber()) {
                        throw jsi::JSError(rt, "setAudioRoute requires route (number)");
                    }
                    int route = jsiutil::safeIntArg(rt, args[0], "route");
                    if (route < static_cast<int>(media::AudioRoute::Earpiece) || route > static_cast<int>(media::AudioRoute::UsbDevice)) {
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

        case MethodId::SetHealthCallback:
            return jsi::Function::createFromHostFunction(rt, name, 1,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (count < 1) {
                        throw jsi::JSError(rt, "setHealthCallback requires a callback function or null");
                    }
                    return setHealthCallback(rt, args[0]);
                });

        case MethodId::GetTrackInfo:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) {
                    return getTrackInfo(rt);
                });

        case MethodId::ResetStream:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) {
                    return resetStream(rt);
                });

        case MethodId::RequestKeyFrame:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) {
                    return requestKeyFrame(rt);
                });

        case MethodId::WarmUp:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) {
                    return warmUp(rt);
                });

        case MethodId::SetKeyFrameNeededCallback:
            return jsi::Function::createFromHostFunction(rt, name, 1,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (count < 1) {
                        throw jsi::JSError(rt, "setKeyFrameNeededCallback requires a callback function or null");
                    }
                    return setKeyFrameNeededCallback(rt, args[0]);
                });

        case MethodId::Pause:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) {
                    return pause(rt);
                });

        case MethodId::Resume:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) {
                    return resume(rt);
                });

        case MethodId::IsPaused:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) {
                    return isPaused(rt);
                });

        case MethodId::GetCurrentTimeUs:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) {
                    return getCurrentTimeUs(rt);
                });

        case MethodId::GetPlaybackState:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) {
                    return getPlaybackState(rt);
                });

        case MethodId::SetBufferTarget:
            return jsi::Function::createFromHostFunction(rt, name, 2,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (count < 2 || !args[0].isNumber() || !args[1].isNumber()) {
                        throw jsi::JSError(rt, "setBufferTarget requires (audioMs: number, videoMs: number)");
                    }
                    return setBufferTarget(rt, args[0].asNumber(), args[1].asNumber());
                });

        case MethodId::SetAudioFocusCallback:
            return jsi::Function::createFromHostFunction(rt, name, 1,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (count < 1) {
                        throw jsi::JSError(rt, "setAudioFocusCallback requires a callback function or null");
                    }
                    return setAudioFocusCallback(rt, args[0]);
                });

        case MethodId::SetCatchupPolicy:
            return jsi::Function::createFromHostFunction(rt, name, 1,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (count < 1 || !args[0].isNumber()) {
                        throw jsi::JSError(rt, "setCatchupPolicy requires (policy: number)");
                    }
                    int val = jsiutil::safeIntArg(rt, args[0], "policy");
                    if (val < 0 || val > 2) {
                        throw jsi::JSError(rt, "setCatchupPolicy: policy out of range (0-2)");
                    }
                    auto policy = static_cast<media::CatchupPolicy>(val);
                    session_.setCatchupPolicy(policy);
                    if (videoQueue_) videoQueue_->setCatchupPolicy(policy);
                    return jsi::Value(true);
                });

        case MethodId::SetStreamStatus:
            return jsi::Function::createFromHostFunction(rt, name, 1,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (count < 1 || !args[0].isNumber()) {
                        throw jsi::JSError(rt, "setStreamStatus requires (status: number)");
                    }
                    int val = jsiutil::safeIntArg(rt, args[0], "status");
                    if (val < 0 || val > 3) {
                        throw jsi::JSError(rt, "setStreamStatus: status out of range (0-3)");
                    }
                    session_.setStreamStatus(static_cast<media::StreamStatus>(val));
                    return jsi::Value(true);
                });

        case MethodId::SetPlaybackRate:
            return jsi::Function::createFromHostFunction(rt, name, 1,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (count < 1 || !args[0].isNumber()) {
                        throw jsi::JSError(rt, "setPlaybackRate requires (rate: number)");
                    }
                    float rate = jsiutil::safeFloatArg(rt, args[0], "rate");
                    rate = std::clamp(rate, media::config::playbackrate::kMinRate,
                                      media::config::playbackrate::kMaxRate);
                    session_.setPlaybackRate(rate);
                    if (videoQueue_) videoQueue_->setPlaybackRate(rate);
                    return jsi::Value(true);
                });

        case MethodId::SetClipBufferDuration:
            return jsi::Function::createFromHostFunction(rt, name, 1,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (count < 1 || !args[0].isNumber()) {
                        throw jsi::JSError(rt, "setClipBufferDuration requires (seconds: number)");
                    }
                    float seconds = static_cast<float>(args[0].asNumber());
                    if (seconds < 0 || seconds > media::config::clip::kMaxDurationSeconds) {
                        throw jsi::JSError(rt, "clip duration must be 0-120 seconds");
                    }
                    size_t bufferSize = static_cast<size_t>(seconds * 150000.0f);
                    if (bufferSize < media::config::clip::kDefaultBufferSize) {
                        bufferSize = media::config::clip::kDefaultBufferSize;
                    }
                    clipBuffer_.setBufferSize(bufferSize);
                    clipBuffer_.setEnabled(seconds > 0);
                    return jsi::Value(true);
                });

        case MethodId::CaptureClip:
            return jsi::Function::createFromHostFunction(rt, name, 1,
                [this](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
                    if (count < 1 || !args[0].isNumber()) {
                        throw jsi::JSError(rt, "captureClip requires (seconds: number)");
                    }
                    float seconds = static_cast<float>(args[0].asNumber());
                    auto weakSelf = weak_from_this();
                    auto callInvoker = callInvoker_;

                    auto constructor = rt.global().getPropertyAsFunction(rt, "Promise");
                    return constructor.callAsConstructor(rt,
                        jsi::Function::createFromHostFunction(rt, jsi::PropNameID::forAscii(rt, "ex"), 2,
                            [weakSelf, seconds, callInvoker](jsi::Runtime& rt, const jsi::Value&,
                                                              const jsi::Value* pArgs, size_t) -> jsi::Value {
                                auto resolve = std::make_shared<jsi::Function>(pArgs[0].getObject(rt).getFunction(rt));
                                auto reject = std::make_shared<jsi::Function>(pArgs[1].getObject(rt).getFunction(rt));
                                auto self = weakSelf.lock();
                                if (!self) {
                                    reject->call(rt, jsi::String::createFromUtf8(rt, "module destroyed"));
                                    return jsi::Value::undefined();
                                }

                                auto clipResult = self->clipBuffer_.extractClip(seconds);
                                if (!clipResult.error.empty()) {
                                    reject->call(rt, jsi::String::createFromUtf8(rt, clipResult.error));
                                    return jsi::Value::undefined();
                                }

                                auto clipData = std::make_shared<std::vector<uint8_t>>(std::move(clipResult.data));
                                std::thread([clipData, resolve, reject, callInvoker, weakSelf]() {
                                    std::string filePath = media::ClipBuffer::generateFilePath();
                                    std::string error;
                                    FILE* f = fopen(filePath.c_str(), "wb");
                                    if (!f) {
                                        error = "failed to create clip file";
                                    } else {
                                        size_t written = fwrite(clipData->data(), 1, clipData->size(), f);
                                        fclose(f);
                                        if (written != clipData->size()) error = "incomplete write";
                                    }
                                    callInvoker->invokeAsync([resolve, reject, filePath, error, weakSelf]() {
                                        auto self = weakSelf.lock();
                                        if (!self || !self->runtime_) return;
                                        auto& rt = *self->runtime_;
                                        if (error.empty()) {
                                            resolve->call(rt, jsi::String::createFromUtf8(rt, filePath));
                                        } else {
                                            reject->call(rt, jsi::String::createFromUtf8(rt, error));
                                        }
                                    });
                                }).detach();
                                return jsi::Value::undefined();
                            }));
                });

        case MethodId::Unknown:
        default:
            return jsi::Value::undefined();
    }
}

std::vector<jsi::PropNameID> MediaPipelineModule::getPropertyNames(jsi::Runtime& rt) {
    std::vector<jsi::PropNameID> names;
    names.reserve(30);
    names.push_back(jsi::PropNameID::forAscii(rt, "feedData"));
    names.push_back(jsi::PropNameID::forAscii(rt, "setMuted"));
    names.push_back(jsi::PropNameID::forAscii(rt, "setGain"));
    names.push_back(jsi::PropNameID::forAscii(rt, "getMetrics"));
    names.push_back(jsi::PropNameID::forAscii(rt, "start"));
    names.push_back(jsi::PropNameID::forAscii(rt, "stop"));
    names.push_back(jsi::PropNameID::forAscii(rt, "isRunning"));
    names.push_back(jsi::PropNameID::forAscii(rt, "setAudioRoute"));
    names.push_back(jsi::PropNameID::forAscii(rt, "getAvailableAudioRoutes"));
    names.push_back(jsi::PropNameID::forAscii(rt, "getAvailableAudioDevices"));
    names.push_back(jsi::PropNameID::forAscii(rt, "getCurrentAudioRoute"));
    names.push_back(jsi::PropNameID::forAscii(rt, "setAudioRouteCallback"));
    names.push_back(jsi::PropNameID::forAscii(rt, "setHealthCallback"));
    names.push_back(jsi::PropNameID::forAscii(rt, "getTrackInfo"));
    names.push_back(jsi::PropNameID::forAscii(rt, "resetStream"));
    names.push_back(jsi::PropNameID::forAscii(rt, "requestKeyFrame"));
    names.push_back(jsi::PropNameID::forAscii(rt, "warmUp"));
    names.push_back(jsi::PropNameID::forAscii(rt, "setKeyFrameNeededCallback"));
    names.push_back(jsi::PropNameID::forAscii(rt, "pause"));
    names.push_back(jsi::PropNameID::forAscii(rt, "resume"));
    names.push_back(jsi::PropNameID::forAscii(rt, "isPaused"));
    names.push_back(jsi::PropNameID::forAscii(rt, "getCurrentTimeUs"));
    names.push_back(jsi::PropNameID::forAscii(rt, "getPlaybackState"));
    names.push_back(jsi::PropNameID::forAscii(rt, "setBufferTarget"));
    names.push_back(jsi::PropNameID::forAscii(rt, "setAudioFocusCallback"));
    names.push_back(jsi::PropNameID::forAscii(rt, "setCatchupPolicy"));
    names.push_back(jsi::PropNameID::forAscii(rt, "setStreamStatus"));
    names.push_back(jsi::PropNameID::forAscii(rt, "setPlaybackRate"));
    names.push_back(jsi::PropNameID::forAscii(rt, "setClipBufferDuration"));
    names.push_back(jsi::PropNameID::forAscii(rt, "captureClip"));
    return names;
}

bool MediaPipelineModule::ensureRunning() {
    if (session_.isRunning()) return true;
    return session_.start();
}

jsi::Value MediaPipelineModule::feedData(jsi::Runtime& rt,
                                          const jsi::Value& bufferVal) {
    if (!session_.isRunning()) return jsi::Value(false);

    BufferView buffer = extractBuffer(rt, bufferVal);
    if (!buffer) return jsi::Value(false);

    clipBuffer_.appendBytes(buffer.data, buffer.size);

    auto result = demuxer_.feedData(buffer.data, buffer.size);

    // Feed stream header to clip buffer once available
    if (demuxer_.hasStreamHeader() && !clipBuffer_.hasStreamHeader()) {
        const auto& hdr = demuxer_.streamHeader();
        clipBuffer_.setStreamHeader(hdr.data(), hdr.size());
    }

    // Track cluster boundaries and keyframes for clip extraction
    if (result.newClusterCount > 0) {
        for (int i = 0; i < result.newClusterCount; ++i) {
            clipBuffer_.onNewCluster();
        }
    }
    for (const auto& pkt : result.videoPackets) {
        if (pkt.isKeyFrame) clipBuffer_.onKeyFrame(pkt.ptsUs);
        clipBuffer_.onBlockPts(pkt.ptsUs);
    }
    for (const auto& pkt : result.audioPackets) {
        clipBuffer_.onBlockPts(pkt.ptsUs);
    }

    for (const auto& pkt : result.audioPackets) {
        session_.pushAudioFrame(pkt.data, pkt.size, pkt.ptsUs, pkt.durationUs);
    }
    if (videoQueue_) {
        for (const auto& pkt : result.videoPackets) {
            videoQueue_->pushEncodedFrame(pkt.data, pkt.size, pkt.ptsUs, pkt.isKeyFrame);
        }
    }

    if (!result.error.empty()) {
        MEDIA_LOG_W("feedData: demux error: %s", result.error.c_str());
        // After overflow or permanent error, trigger keyframe recovery so the
        // video pipeline doesn't stall waiting for frames that were lost.
        if (videoQueue_) {
            videoQueue_->requestKeyFrame();
        }
        media::VideoSurfaceRegistry::instance().requestKeyFrame();
        return jsi::Value(false);
    }
    return jsi::Value(true);
}

jsi::Value MediaPipelineModule::setMuted(jsi::Runtime&, bool muted) {
    session_.setMuted(muted);
    return jsi::Value(true);
}

jsi::Value MediaPipelineModule::setGain(jsi::Runtime&, float gain) {
    session_.setGain(gain);
    return jsi::Value(true);
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

    jsi::Object quality(rt);
    quality.setProperty(rt, "framesReceived", static_cast<double>(metrics.audioFramesReceived));
    quality.setProperty(rt, "underruns", static_cast<double>(metrics.underruns));
    quality.setProperty(rt, "framesDropped", static_cast<double>(metrics.framesDropped));
    quality.setProperty(rt, "framesDrained", static_cast<double>(metrics.framesDrained));
    quality.setProperty(rt, "decodeErrors", static_cast<double>(metrics.decodeErrors));
    quality.setProperty(rt, "decoderResets", static_cast<double>(metrics.decoderResets));
    quality.setProperty(rt, "ptsDiscontinuities", static_cast<double>(metrics.ptsDiscontinuities));
    quality.setProperty(rt, "fastPathSwitches", static_cast<double>(metrics.fastPathSwitches));
    quality.setProperty(rt, "catchupDeadZoneSnaps", static_cast<double>(metrics.catchupDeadZoneSnaps));
    quality.setProperty(rt, "maxInterFrameGapMs", static_cast<double>(metrics.maxInterFrameGapUs) / 1000.0);
    quality.setProperty(rt, "gapsOver50ms", static_cast<double>(metrics.gapsOver50ms));
    quality.setProperty(rt, "gapsOver100ms", static_cast<double>(metrics.gapsOver100ms));
    quality.setProperty(rt, "gapsOver500ms", static_cast<double>(metrics.gapsOver500ms));
    result.setProperty(rt, "quality", std::move(quality));

    jsi::Object session(rt);
    session.setProperty(rt, "uptimeMs", static_cast<double>(metrics.uptimeUs / 1000));
    session.setProperty(rt, "samplesOutput", static_cast<double>(metrics.totalSamplesOutput));
    session.setProperty(rt, "playbackRate", static_cast<double>(session_.playbackRate()));
    result.setProperty(rt, "session", std::move(session));

    // Latency mode reporting
    jsi::Object latency(rt);
    latency.setProperty(rt, "mode", media::latencyModeToString(metrics.grantedLatencyMode));
    latency.setProperty(rt, "isLowestLatency", metrics.grantedLatencyMode == media::LatencyMode::LowLatency);
    result.setProperty(rt, "latency", std::move(latency));

    // Pool pressure reporting for backpressure signaling
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

    // Jitter buffer metrics
    jsi::Object jitterObj(rt);
    jitterObj.setProperty(rt, "jitterUs", static_cast<double>(metrics.jitterUs));
    jitterObj.setProperty(rt, "bufferTargetUs", static_cast<double>(metrics.bufferTargetUs));
    result.setProperty(rt, "jitter", std::move(jitterObj));

    // Drift compensation
    jsi::Object drift(rt);
    drift.setProperty(rt, "driftPpm", metrics.driftPpm);
    drift.setProperty(rt, "active", metrics.driftCompensationActive);
    drift.setProperty(rt, "currentRatio", static_cast<double>(metrics.driftCurrentRatio));
    drift.setProperty(rt, "catchupRatio", static_cast<double>(metrics.catchupCurrentRatio));
    result.setProperty(rt, "drift", std::move(drift));

    // Audio pipeline state (triage observability)
    jsi::Object pipeline(rt);
    pipeline.setProperty(rt, "audioStreamState", static_cast<int>(metrics.audioStreamState));
    pipeline.setProperty(rt, "currentGain", static_cast<double>(metrics.currentGain));
    pipeline.setProperty(rt, "bufferedDurationUs", static_cast<double>(metrics.bufferedDurationUs));
    pipeline.setProperty(rt, "decodedDurationUs", static_cast<double>(metrics.decodedDurationUs));
    result.setProperty(rt, "pipeline", std::move(pipeline));

    // Demuxer metrics
    jsi::Object demux(rt);
    auto dm = demuxer_.demuxerMetrics();
    demux.setProperty(rt, "totalBytesFed", static_cast<double>(dm.totalBytesFed));
    demux.setProperty(rt, "feedDataCalls", static_cast<double>(dm.feedDataCalls));
    demux.setProperty(rt, "audioPacketsEmitted", static_cast<double>(dm.audioPacketsEmitted));
    demux.setProperty(rt, "videoPacketsEmitted", static_cast<double>(dm.videoPacketsEmitted));
    demux.setProperty(rt, "overflowCount", static_cast<double>(dm.overflowCount));
    demux.setProperty(rt, "parseState", static_cast<int>(dm.parseState));
    result.setProperty(rt, "demux", std::move(demux));

    // Video metrics (from VideoFrameQueue + AVSyncCoordinator)
    jsi::Object video(rt);
    if (videoQueue_) {
        const auto& vm = videoQueue_->metrics();
        video.setProperty(rt, "framesReceived", static_cast<double>(vm.framesReceived.load(std::memory_order_relaxed)));
        video.setProperty(rt, "framesDecoded", static_cast<double>(vm.framesDecoded.load(std::memory_order_relaxed)));
        video.setProperty(rt, "framesDropped", static_cast<double>(vm.framesDropped.load(std::memory_order_relaxed)));
        video.setProperty(rt, "decodeErrors", static_cast<double>(vm.decodeErrors.load(std::memory_order_relaxed)));
        video.setProperty(rt, "currentFps", static_cast<double>(vm.currentFps.load(std::memory_order_relaxed)));
        video.setProperty(rt, "width", static_cast<int>(vm.width.load(std::memory_order_relaxed)));
        video.setProperty(rt, "height", static_cast<int>(vm.height.load(std::memory_order_relaxed)));
        video.setProperty(rt, "jitterUs", static_cast<double>(vm.jitterUs.load(std::memory_order_relaxed)));
        video.setProperty(rt, "bufferTargetUs", static_cast<double>(vm.bufferTargetUs.load(std::memory_order_relaxed)));
        video.setProperty(rt, "lateFrames", static_cast<double>(vm.lateFrames.load(std::memory_order_relaxed)));
        video.setProperty(rt, "skippedFrames", static_cast<double>(vm.skippedFrames.load(std::memory_order_relaxed)));
        video.setProperty(rt, "decoderResets", static_cast<double>(vm.decoderResets.load(std::memory_order_relaxed)));
        video.setProperty(rt, "surfaceLostCount", static_cast<double>(vm.surfaceLostCount.load(std::memory_order_relaxed)));
        video.setProperty(rt, "maxInterFrameGapMs", static_cast<double>(vm.maxInterFrameGapUs.load(std::memory_order_relaxed)) / 1000.0);
        video.setProperty(rt, "gapsOver50ms", static_cast<double>(vm.gapsOver50ms.load(std::memory_order_relaxed)));
        video.setProperty(rt, "gapsOver100ms", static_cast<double>(vm.gapsOver100ms.load(std::memory_order_relaxed)));
        video.setProperty(rt, "gapsOver500ms", static_cast<double>(vm.gapsOver500ms.load(std::memory_order_relaxed)));
        video.setProperty(rt, "driftPpm", static_cast<double>(vm.driftPpm.load(std::memory_order_relaxed)));
        video.setProperty(rt, "queueDepth", static_cast<int>(videoQueue_->pendingFrames()));
        video.setProperty(rt, "needsKeyFrame", videoQueue_->isAwaitingKeyFrame());
    } else {
        video.setProperty(rt, "framesReceived", 0.0);
        video.setProperty(rt, "framesDecoded", 0.0);
        video.setProperty(rt, "framesDropped", 0.0);
        video.setProperty(rt, "decodeErrors", 0.0);
        video.setProperty(rt, "currentFps", 0.0);
        video.setProperty(rt, "width", 0);
        video.setProperty(rt, "height", 0);
        video.setProperty(rt, "jitterUs", 0.0);
        video.setProperty(rt, "bufferTargetUs", 0.0);
        video.setProperty(rt, "lateFrames", 0.0);
        video.setProperty(rt, "skippedFrames", 0.0);
        video.setProperty(rt, "decoderResets", 0.0);
        video.setProperty(rt, "surfaceLostCount", 0.0);
        video.setProperty(rt, "maxInterFrameGapMs", 0.0);
        video.setProperty(rt, "gapsOver50ms", 0.0);
        video.setProperty(rt, "gapsOver100ms", 0.0);
        video.setProperty(rt, "gapsOver500ms", 0.0);
        video.setProperty(rt, "driftPpm", 0.0);
        video.setProperty(rt, "queueDepth", 0);
        video.setProperty(rt, "needsKeyFrame", false);
    }
    video.setProperty(rt, "avSyncOffsetUs", syncCoordinator_
        ? static_cast<double>(syncCoordinator_->currentOffsetUs())
        : 0.0);
    result.setProperty(rt, "video", std::move(video));

    return result;
}

jsi::Value MediaPipelineModule::start(jsi::Runtime&) {
    std::lock_guard<std::mutex> lk(lifecycleMtx_);
    return jsi::Value(ensureRunning());
}

jsi::Value MediaPipelineModule::stop(jsi::Runtime&) {
    std::lock_guard<std::mutex> lk(lifecycleMtx_);
    session_.stop();
    demuxer_.reset();
    return jsi::Value(true);
}

jsi::Value MediaPipelineModule::isRunning(jsi::Runtime&) {
    return jsi::Value(session_.isRunning());
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
        auto availableDevices = mgr.getAvailableAudioDevices();

        jsi::Object event(rt);
        event.setProperty(rt, "route", static_cast<int>(currentRoute));

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

jsi::Value MediaPipelineModule::setHealthCallback(jsi::Runtime& rt, const jsi::Value& callback) {
    if (callback.isNull() || callback.isUndefined()) {
        healthCallback_.reset();
        MEDIA_LOG_I("MediaPipelineModule: health callback cleared");
        return jsi::Value(true);
    }

    if (!callback.isObject() || !callback.asObject(rt).isFunction(rt)) {
        throw jsi::JSError(rt, "setHealthCallback requires a function or null");
    }

    healthCallback_ = std::make_shared<jsi::Function>(callback.asObject(rt).asFunction(rt));
    setupHealthWatchdog();

    MEDIA_LOG_I("MediaPipelineModule: health callback set");
    return jsi::Value(true);
}

void MediaPipelineModule::setupHealthWatchdog() {
    if (!healthCallback_ || !callInvoker_) return;

    auto weakSelf = weak_from_this();

    auto readMetrics = [weakSelf]() -> media::HealthSnapshot {
        media::HealthSnapshot snap{};
        auto self = weakSelf.lock();
        if (!self) return snap;

        if (self->session_.isRunning()) {
            const auto& m = self->session_.audioMetrics();
            snap.underruns = m.underruns.load(std::memory_order_relaxed);
            snap.decodeErrors = m.decodeErrors.load(std::memory_order_relaxed);
            snap.framesDropped = m.framesDropped.load(std::memory_order_relaxed);
            snap.ptsDiscontinuities = m.ptsDiscontinuities.load(std::memory_order_relaxed);
            snap.decoderResets = m.decoderResets.load(std::memory_order_relaxed);
            snap.gapsOver500ms = m.gapsOver500ms.load(std::memory_order_relaxed);
            snap.gapsOver100ms = m.gapsOver100ms.load(std::memory_order_relaxed);
            snap.audioFramesReceived = m.framesReceived.load(std::memory_order_relaxed);
            snap.audioSamplesOutput = m.samplesOutput.load(std::memory_order_relaxed);
        }
        snap.paused = self->session_.isPaused();
        snap.bufferedDurationUs = self->session_.bufferedDurationUs();
        snap.decodedDurationUs = self->session_.decodedDurationUs();
        snap.uptimeUs = self->session_.uptimeUs();
        snap.decodeThreadDetached = self->session_.wasDecodeThreadDetached();
        snap.audioOutputRunning = self->session_.isAudioOutputRunning();
        if (self->syncCoordinator_) {
            snap.avSyncOffsetUs = self->syncCoordinator_->currentOffsetUs();
        }
        snap.bufferTargetUs = self->session_.bufferTargetUs();
        snap.streamStatus = self->session_.streamStatus();
        if (self->videoQueue_) {
            const auto& vm = self->videoQueue_->metrics();
            snap.videoFramesReceived = vm.framesReceived.load(std::memory_order_relaxed);
            snap.videoFramesDecoded = vm.framesDecoded.load(std::memory_order_relaxed);
            snap.videoFramesDropped = vm.framesDropped.load(std::memory_order_relaxed);
        }
        return snap;
    };

    auto onHealthChange = [weakSelf](const media::HealthEvent& event) {
        auto self = weakSelf.lock();
        if (!self || !self->healthCallback_ || !self->callInvoker_ || !self->runtime_) return;

        int statusInt = static_cast<int>(event.status);
        std::string detail(event.detail);
        auto snapshot = event.metrics;
        int streamStatusInt = static_cast<int>(snapshot.streamStatus);

        self->callInvoker_->invokeAsync([weakSelf, statusInt, detail, snapshot, streamStatusInt]() {
            auto self = weakSelf.lock();
            if (!self || !self->runtime_ || !self->healthCallback_) return;
            try {
                auto& rt = *self->runtime_;
                jsi::Object ev(rt);
                ev.setProperty(rt, "status", statusInt);
                ev.setProperty(rt, "detail", jsi::String::createFromUtf8(rt, detail));
                ev.setProperty(rt, "streamStatus", streamStatusInt);

                jsi::Object metricsObj(rt);
                metricsObj.setProperty(rt, "underruns", static_cast<double>(snapshot.underruns));
                metricsObj.setProperty(rt, "decodeErrors", static_cast<double>(snapshot.decodeErrors));
                metricsObj.setProperty(rt, "framesDropped", static_cast<double>(snapshot.framesDropped));
                metricsObj.setProperty(rt, "avSyncOffsetUs", static_cast<double>(snapshot.avSyncOffsetUs));
                metricsObj.setProperty(rt, "bufferTargetUs", static_cast<double>(snapshot.bufferTargetUs));
                metricsObj.setProperty(rt, "gapsOver100ms", static_cast<double>(snapshot.gapsOver100ms));
                metricsObj.setProperty(rt, "gapsOver500ms", static_cast<double>(snapshot.gapsOver500ms));
                metricsObj.setProperty(rt, "ptsDiscontinuities", static_cast<double>(snapshot.ptsDiscontinuities));
                metricsObj.setProperty(rt, "decoderResets", static_cast<double>(snapshot.decoderResets));
                metricsObj.setProperty(rt, "videoFramesReceived", static_cast<double>(snapshot.videoFramesReceived));
                metricsObj.setProperty(rt, "videoFramesDecoded", static_cast<double>(snapshot.videoFramesDecoded));
                metricsObj.setProperty(rt, "videoFramesDropped", static_cast<double>(snapshot.videoFramesDropped));
                ev.setProperty(rt, "metrics", std::move(metricsObj));

                self->healthCallback_->call(rt, ev);
            } catch (const std::exception& e) {
                MEDIA_LOG_E("MediaPipelineModule: health callback error: %s", e.what());
            }
        });
    };

    session_.setHealthWatchdog(
        std::make_unique<media::HealthWatchdog>(std::move(readMetrics), std::move(onHealthChange)));
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
            self->session_.requestDriftReset();
        }
    });

    // Route callback: RouteHandler fires this via fireJsCallback.
    // Dedup is handled by RouteHandler. We forward to JS via CallInvoker.
    mgr.setRouteCallback(
        [weakSelf](media::AudioRoute route) {
            auto self = weakSelf.lock();
            if (!self || !self->routeCallback_ || !self->callInvoker_ || !self->runtime_) {
                return;
            }

            int routeInt = static_cast<int>(route);
            auto availableDevices = media::AudioSessionManager::instance().getAvailableAudioDevices();

            self->callInvoker_->invokeAsync([weakSelf, routeInt, availableDevices]() {
                auto self = weakSelf.lock();
                if (!self || !self->runtime_ || !self->routeCallback_) return;
                try {
                    auto& rt = *self->runtime_;
                    jsi::Object event(rt);
                    event.setProperty(rt, "route", routeInt);

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

jsi::Value MediaPipelineModule::getTrackInfo(jsi::Runtime& rt) {
    if (demuxer_.parseState() != media::demux::ParseState::Streaming) {
        return jsi::Value::null();
    }
    const auto& info = demuxer_.trackInfo();
    jsi::Object obj(rt);
    obj.setProperty(rt, "audioCodecId", jsi::String::createFromUtf8(rt, info.audioCodecId));
    obj.setProperty(rt, "videoCodecId", jsi::String::createFromUtf8(rt, info.videoCodecId));
    obj.setProperty(rt, "videoWidth", info.videoWidth);
    obj.setProperty(rt, "videoHeight", info.videoHeight);
    return obj;
}

jsi::Value MediaPipelineModule::resetStream(jsi::Runtime&) {
    std::lock_guard<std::mutex> lk(lifecycleMtx_);
    demuxer_.reset();
    clipBuffer_.reset();
    session_.resetStreamState();
    if (videoQueue_) {
        videoQueue_->reset();
    }
    return jsi::Value(true);
}

jsi::Value MediaPipelineModule::requestKeyFrame(jsi::Runtime&) {
    if (videoQueue_) {
        videoQueue_->requestKeyFrame();
        return jsi::Value(true);
    }
    return jsi::Value(false);
}

jsi::Value MediaPipelineModule::warmUp(jsi::Runtime&) {
    std::lock_guard<std::mutex> lk(lifecycleMtx_);
    return jsi::Value(session_.warmUp());
}

jsi::Value MediaPipelineModule::setKeyFrameNeededCallback(jsi::Runtime& rt, const jsi::Value& callback) {
    if (callback.isNull() || callback.isUndefined()) {
        keyFrameNeededCallback_.reset();
        media::VideoSurfaceRegistry::instance().setKeyFrameRequestFn(nullptr);
        MEDIA_LOG_I("MediaPipelineModule: keyframe needed callback cleared");
        return jsi::Value(true);
    }

    if (!callback.isObject() || !callback.asObject(rt).isFunction(rt)) {
        throw jsi::JSError(rt, "setKeyFrameNeededCallback requires a function or null");
    }

    keyFrameNeededCallback_ = std::make_shared<jsi::Function>(callback.asObject(rt).asFunction(rt));
    setupKeyFrameNeededCallback();

    MEDIA_LOG_I("MediaPipelineModule: keyframe needed callback set");
    return jsi::Value(true);
}

void MediaPipelineModule::setupKeyFrameNeededCallback() {
    if (!keyFrameNeededCallback_ || !callInvoker_) return;

    auto weakSelf = weak_from_this();

    media::VideoSurfaceRegistry::instance().setKeyFrameRequestFn(
        [weakSelf]() {
            auto self = weakSelf.lock();
            if (!self || !self->keyFrameNeededCallback_ || !self->callInvoker_ || !self->runtime_) {
                return;
            }

            self->callInvoker_->invokeAsync([weakSelf]() {
                auto self = weakSelf.lock();
                if (!self || !self->runtime_ || !self->keyFrameNeededCallback_) return;
                try {
                    self->keyFrameNeededCallback_->call(*self->runtime_);
                } catch (const std::exception& e) {
                    MEDIA_LOG_E("MediaPipelineModule: keyframe needed callback error: %s", e.what());
                }
            });
        });
}

jsi::Value MediaPipelineModule::pause(jsi::Runtime&) {
    return jsi::Value(session_.pause());
}

jsi::Value MediaPipelineModule::resume(jsi::Runtime&) {
    return jsi::Value(session_.resume());
}

jsi::Value MediaPipelineModule::isPaused(jsi::Runtime&) {
    return jsi::Value(session_.isPaused());
}

jsi::Value MediaPipelineModule::getCurrentTimeUs(jsi::Runtime&) {
    return jsi::Value(static_cast<double>(session_.currentTimeUs()));
}

jsi::Value MediaPipelineModule::getPlaybackState(jsi::Runtime&) {
    // Idle=0, Buffering=1, Playing=2, Paused=3, Stalled=4, Failed=5
    if (!session_.isRunning()) return jsi::Value(0);
    if (session_.isPaused()) return jsi::Value(3);
    auto audioState = session_.audioState();
    if (audioState == media::StreamState::Buffering || audioState == media::StreamState::Underrun) {
        return jsi::Value(1);
    }
    return jsi::Value(2);  // Playing
}

jsi::Value MediaPipelineModule::setBufferTarget(jsi::Runtime&, double audioMs, double videoMs) {
    int64_t audioUs = (audioMs > 0) ? static_cast<int64_t>(audioMs * 1000.0) : 0;
    int64_t videoUs = (videoMs > 0) ? static_cast<int64_t>(videoMs * 1000.0) : 0;
    session_.setBufferTargetOverride(audioUs, videoUs);
    if (videoQueue_ && videoUs > 0) {
        videoQueue_->setBufferTargetOverride(videoUs);
    } else if (videoQueue_ && videoUs == 0) {
        videoQueue_->setBufferTargetOverride(0);
    }
    return jsi::Value(true);
}

jsi::Value MediaPipelineModule::setAudioFocusCallback(jsi::Runtime& rt, const jsi::Value& callback) {
    if (callback.isNull() || callback.isUndefined()) {
        audioFocusCallback_.reset();
        MEDIA_LOG_I("MediaPipelineModule: audio focus callback cleared");
        return jsi::Value(true);
    }

    if (!callback.isObject() || !callback.asObject(rt).isFunction(rt)) {
        throw jsi::JSError(rt, "setAudioFocusCallback requires a function or null");
    }

    audioFocusCallback_ = std::make_shared<jsi::Function>(callback.asObject(rt).asFunction(rt));
    MEDIA_LOG_I("MediaPipelineModule: audio focus callback set");
    return jsi::Value(true);
}

}  // namespace mediamodule
