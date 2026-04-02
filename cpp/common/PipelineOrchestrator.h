// Shared pipeline orchestration logic for iOS and Android MediaPipelineModule.
//
// INCLUDE ORDER: Platform files MUST include MediaSession.h and AudioSessionManager.h
// BEFORE this header. These types differ per platform but share the same interface.
//
// Threading: All methods called from JS thread only (via JSI HostObject::get).
#pragma once

#include <jsi/jsi.h>
#include <ReactCommon/CallInvoker.h>
#include <mutex>
#include <memory>
#include <vector>
#include <string>
#include <thread>
#include <cmath>
#include <cstdio>
#include "MediaLog.h"
#include "MediaConfig.h"
#include "JsiUtils.h"
#include "AudioRouteTypes.h"
#include "HealthWatchdog.h"
#include "ClipBuffer.h"
#include "StallRecoveryController.h"
#include "demux/WebmDemuxer.h"
#include "video/VideoFrameQueue.h"
#include "video/VideoSurfaceRegistry.h"

namespace mediamodule {

namespace jsi = facebook::jsi;

enum class MethodId : uint8_t {
    Unknown = 0, FeedData, SetMuted, SetGain, GetMetrics, Start, Stop, IsRunning,
    SetAudioRoute, GetAvailableAudioRoutes, GetAvailableAudioDevices, GetCurrentAudioRoute,
    SetAudioRouteCallback, SetHealthCallback, GetTrackInfo, ResetStream, RequestKeyFrame,
    WarmUp, SetKeyFrameNeededCallback, Pause, Resume, IsPaused, GetCurrentTimeUs,
    GetPlaybackState, SetBufferTarget, SetAudioFocusCallback, SetCatchupPolicy,
    SetStreamStatus, SetPlaybackRate, SetClipBufferDuration, CaptureClip, SeekTo,
    GetBufferRangeSeconds
};

inline MethodId lookupMethod(const std::string& name) noexcept {
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
    if (name == "seekTo") return MethodId::SeekTo;
    if (name == "getBufferRangeSeconds") return MethodId::GetBufferRangeSeconds;
    return MethodId::Unknown;
}

// All shared pipeline logic. Platform MediaPipelineModule owns one of these
// and forwards HostObject::get()/getPropertyNames() to it.
//
// WeakOwner is a type-erased weak_ptr to the owning module, used in
// callback closures to check liveness without coupling to the concrete type.
class PipelineOrchestrator {
public:
    using WeakOwner = std::weak_ptr<void>;

    ~PipelineOrchestrator() { joinClipThread(); }

    void init(std::shared_ptr<facebook::react::CallInvoker> invoker,
              jsi::Runtime* rt, WeakOwner owner) noexcept {
        callInvoker_ = std::move(invoker);
        runtime_ = rt;
        weakOwner_ = std::move(owner);
    }

    void setSyncCoordinator(media::AVSyncCoordinator* c) noexcept {
        syncCoordinator_ = c;
        session_.setSyncCoordinator(c);
    }

    void setVideoQueue(media::VideoFrameQueue* q) noexcept {
        videoQueue_ = q;
        if (q) {
            media::VideoSurfaceRegistry::instance().setSurfaceLostFn([q]() {
                q->metrics().surfaceLostCount.fetch_add(1, std::memory_order_relaxed);
                q->requestKeyFrame();
            });
        }
    }

    void teardown() noexcept {
        session_.stop();
        joinClipThread();
        media::AudioSessionManager::instance().setRouteCallback(nullptr);
        routeCallback_.reset();
        healthCallback_.reset();
        keyFrameNeededCallback_.reset();
        audioFocusCallback_.reset();
        media::VideoSurfaceRegistry::instance().setKeyFrameRequestFn(nullptr);
        runtime_ = nullptr;
    }

    media::MediaSession& session() noexcept { return session_; }
    media::demux::WebmDemuxer& demuxer() noexcept { return demuxer_; }

    jsi::Value get(jsi::Runtime& rt, const jsi::PropNameID& name) {
        std::string n = name.utf8(rt);
        switch (lookupMethod(n)) {
        case MethodId::FeedData:
            return jsi::Function::createFromHostFunction(rt, name, 1,
                [this, weak = weakOwner_](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (!weak.lock()) return jsi::Value(false);
                    if (count < 1) throw jsi::JSError(rt, "feedData requires (buffer)");
                    return feedData(rt, args[0]);
                });
        case MethodId::SetMuted:
            return jsi::Function::createFromHostFunction(rt, name, 1,
                [this, weak = weakOwner_](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (!weak.lock()) return jsi::Value(false);
                    if (count < 1 || !args[0].isBool()) throw jsi::JSError(rt, "setMuted requires (muted: boolean)");
                    session_.setMuted(args[0].getBool());
                    return jsi::Value(true);
                });
        case MethodId::SetGain:
            return jsi::Function::createFromHostFunction(rt, name, 1,
                [this, weak = weakOwner_](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (!weak.lock()) return jsi::Value(false);
                    if (count < 1 || !args[0].isNumber()) throw jsi::JSError(rt, "setGain requires (gain: number)");
                    session_.setGain(jsiutil::safeFloatArg(rt, args[0], "gain"));
                    return jsi::Value(true);
                });
        case MethodId::GetMetrics:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [this, weak = weakOwner_](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) {
                    if (!weak.lock()) return jsi::Value::undefined();
                    return getMetrics(rt);
                });
        case MethodId::Start:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [this, weak = weakOwner_](jsi::Runtime&, const jsi::Value&, const jsi::Value*, size_t) {
                    if (!weak.lock()) return jsi::Value(false);
                    std::lock_guard<std::mutex> lk(lifecycleMtx_);
                    return jsi::Value(ensureRunning());
                });
        case MethodId::Stop:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [this, weak = weakOwner_](jsi::Runtime&, const jsi::Value&, const jsi::Value*, size_t) {
                    if (!weak.lock()) return jsi::Value(false);
                    std::lock_guard<std::mutex> lk(lifecycleMtx_);
                    session_.stop();
                    demuxer_.reset();
                    return jsi::Value(true);
                });
        case MethodId::IsRunning:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [this, weak = weakOwner_](jsi::Runtime&, const jsi::Value&, const jsi::Value*, size_t) {
                    if (!weak.lock()) return jsi::Value(false);
                    return jsi::Value(session_.isRunning());
                });
        case MethodId::SetAudioRoute:
            return jsi::Function::createFromHostFunction(rt, name, 2,
                [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (count < 1 || !args[0].isNumber()) throw jsi::JSError(rt, "setAudioRoute requires route (number)");
                    int route = jsiutil::safeIntArg(rt, args[0], "route");
                    if (route < static_cast<int>(media::AudioRoute::Earpiece) || route > static_cast<int>(media::AudioRoute::UsbDevice))
                        throw jsi::JSError(rt, "setAudioRoute: route out of range");
                    std::string deviceId;
                    if (count >= 2 && args[1].isString()) deviceId = args[1].getString(rt).utf8(rt);
                    return jsi::Value(media::AudioSessionManager::instance().setAudioRoute(
                        static_cast<media::AudioRoute>(route), deviceId));
                });
        case MethodId::GetAvailableAudioRoutes:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) {
                    auto routes = media::AudioSessionManager::instance().getAvailableAudioRoutes();
                    jsi::Array result(rt, routes.size());
                    for (size_t i = 0; i < routes.size(); ++i)
                        result.setValueAtIndex(rt, i, static_cast<int>(routes[i]));
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
                        obj.setProperty(rt, "deviceName", jsi::String::createFromUtf8(rt, devices[i].deviceName));
                        obj.setProperty(rt, "deviceId", jsi::String::createFromUtf8(rt, devices[i].deviceId));
                        result.setValueAtIndex(rt, i, std::move(obj));
                    }
                    return result;
                });
        case MethodId::GetCurrentAudioRoute:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [](jsi::Runtime&, const jsi::Value&, const jsi::Value*, size_t) {
                    media::AudioSessionManager::instance().ensureRouteDetected();
                    return jsi::Value(static_cast<int>(media::AudioSessionManager::instance().currentRoute()));
                });
        case MethodId::SetAudioRouteCallback:
            return jsi::Function::createFromHostFunction(rt, name, 1,
                [this, weak = weakOwner_](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (!weak.lock()) return jsi::Value(false);
                    if (count < 1) throw jsi::JSError(rt, "setAudioRouteCallback requires a callback function or null");
                    return setAudioRouteCallback(rt, args[0]);
                });
        case MethodId::SetHealthCallback:
            return jsi::Function::createFromHostFunction(rt, name, 1,
                [this, weak = weakOwner_](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (!weak.lock()) return jsi::Value(false);
                    if (count < 1) throw jsi::JSError(rt, "setHealthCallback requires a callback function or null");
                    return setHealthCallback(rt, args[0]);
                });
        case MethodId::GetTrackInfo:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [this, weak = weakOwner_](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) {
                    if (!weak.lock()) return jsi::Value::null();
                    return getTrackInfo(rt);
                });
        case MethodId::ResetStream:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [this, weak = weakOwner_](jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, size_t) {
                    if (!weak.lock()) return jsi::Value(false);
                    return resetStream(rt);
                });
        case MethodId::RequestKeyFrame:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [this, weak = weakOwner_](jsi::Runtime&, const jsi::Value&, const jsi::Value*, size_t) {
                    if (!weak.lock()) return jsi::Value(false);
                    if (videoQueue_) { videoQueue_->requestKeyFrame(); return jsi::Value(true); }
                    return jsi::Value(false);
                });
        case MethodId::WarmUp:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [this, weak = weakOwner_](jsi::Runtime&, const jsi::Value&, const jsi::Value*, size_t) {
                    if (!weak.lock()) return jsi::Value(false);
                    std::lock_guard<std::mutex> lk(lifecycleMtx_);
                    return jsi::Value(session_.warmUp());
                });
        case MethodId::SetKeyFrameNeededCallback:
            return jsi::Function::createFromHostFunction(rt, name, 1,
                [this, weak = weakOwner_](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (!weak.lock()) return jsi::Value(false);
                    if (count < 1) throw jsi::JSError(rt, "setKeyFrameNeededCallback requires a callback function or null");
                    return setKeyFrameNeededCallback(rt, args[0]);
                });
        case MethodId::Pause:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [this, weak = weakOwner_](jsi::Runtime&, const jsi::Value&, const jsi::Value*, size_t) {
                    if (!weak.lock()) return jsi::Value(false);
                    return jsi::Value(session_.pause());
                });
        case MethodId::Resume:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [this, weak = weakOwner_](jsi::Runtime&, const jsi::Value&, const jsi::Value*, size_t) {
                    if (!weak.lock()) return jsi::Value(false);
                    return jsi::Value(session_.resume());
                });
        case MethodId::IsPaused:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [this, weak = weakOwner_](jsi::Runtime&, const jsi::Value&, const jsi::Value*, size_t) {
                    if (!weak.lock()) return jsi::Value(false);
                    return jsi::Value(session_.isPaused());
                });
        case MethodId::GetCurrentTimeUs:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [this, weak = weakOwner_](jsi::Runtime&, const jsi::Value&, const jsi::Value*, size_t) {
                    if (!weak.lock()) return jsi::Value(0.0);
                    return jsi::Value(static_cast<double>(session_.currentTimeUs()));
                });
        case MethodId::GetPlaybackState:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [this, weak = weakOwner_](jsi::Runtime&, const jsi::Value&, const jsi::Value*, size_t) {
                    if (!weak.lock()) return jsi::Value(0);
                    if (!session_.isRunning()) return jsi::Value(0);   // Idle
                    if (session_.isPaused()) return jsi::Value(3);     // Paused
                    auto stallState = stallRecovery_.state();
                    if (stallState == media::StallState::Failed) return jsi::Value(5);  // Failed
                    if (stallState == media::StallState::Stalled || stallState == media::StallState::Recovering) return jsi::Value(4);  // Stalled
                    auto s = session_.audioState();
                    if (s == media::StreamState::Buffering || s == media::StreamState::Underrun) return jsi::Value(1);  // Buffering
                    return jsi::Value(2);  // Playing
                });
        case MethodId::SetBufferTarget:
            return jsi::Function::createFromHostFunction(rt, name, 2,
                [this, weak = weakOwner_](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (!weak.lock()) return jsi::Value(false);
                    if (count < 2 || !args[0].isNumber() || !args[1].isNumber())
                        throw jsi::JSError(rt, "setBufferTarget requires (audioMs: number, videoMs: number)");
                    double audioMs = args[0].asNumber(), videoMs = args[1].asNumber();
                    int64_t audioUs = (audioMs > 0) ? static_cast<int64_t>(audioMs * 1000.0) : 0;
                    int64_t videoUs = (videoMs > 0) ? static_cast<int64_t>(videoMs * 1000.0) : 0;
                    session_.setBufferTargetOverride(audioUs, videoUs);
                    if (videoQueue_) videoQueue_->setBufferTargetOverride(videoUs);
                    return jsi::Value(true);
                });
        case MethodId::SetAudioFocusCallback:
            return jsi::Function::createFromHostFunction(rt, name, 1,
                [this, weak = weakOwner_](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (!weak.lock()) return jsi::Value(false);
                    if (count < 1) throw jsi::JSError(rt, "setAudioFocusCallback requires a callback function or null");
                    return setAudioFocusCallback(rt, args[0]);
                });
        case MethodId::SetCatchupPolicy:
            return jsi::Function::createFromHostFunction(rt, name, 1,
                [this, weak = weakOwner_](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (!weak.lock()) return jsi::Value(false);
                    if (count < 1 || !args[0].isNumber()) throw jsi::JSError(rt, "setCatchupPolicy requires (policy: number)");
                    int val = jsiutil::safeIntArg(rt, args[0], "policy");
                    if (val < 0 || val > 2) throw jsi::JSError(rt, "setCatchupPolicy: policy out of range (0-2)");
                    auto policy = static_cast<media::CatchupPolicy>(val);
                    session_.setCatchupPolicy(policy);
                    if (videoQueue_) videoQueue_->setCatchupPolicy(policy);
                    return jsi::Value(true);
                });
        case MethodId::SetStreamStatus:
            return jsi::Function::createFromHostFunction(rt, name, 1,
                [this, weak = weakOwner_](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (!weak.lock()) return jsi::Value(false);
                    if (count < 1 || !args[0].isNumber()) throw jsi::JSError(rt, "setStreamStatus requires (status: number)");
                    int val = jsiutil::safeIntArg(rt, args[0], "status");
                    if (val < 0 || val > 3) throw jsi::JSError(rt, "setStreamStatus: status out of range (0-3)");
                    session_.setStreamStatus(static_cast<media::StreamStatus>(val));
                    return jsi::Value(true);
                });
        case MethodId::SetPlaybackRate:
            return jsi::Function::createFromHostFunction(rt, name, 1,
                [this, weak = weakOwner_](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (!weak.lock()) return jsi::Value(false);
                    if (count < 1 || !args[0].isNumber()) throw jsi::JSError(rt, "setPlaybackRate requires (rate: number)");
                    float rate = jsiutil::safeFloatArg(rt, args[0], "rate");
                    rate = std::clamp(rate, media::config::playbackrate::kMinRate, media::config::playbackrate::kMaxRate);
                    session_.setPlaybackRate(rate);
                    if (videoQueue_) videoQueue_->setPlaybackRate(rate);
                    return jsi::Value(true);
                });
        case MethodId::SetClipBufferDuration:
            return jsi::Function::createFromHostFunction(rt, name, 1,
                [this, weak = weakOwner_](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (!weak.lock()) return jsi::Value(false);
                    if (count < 1 || !args[0].isNumber()) throw jsi::JSError(rt, "setClipBufferDuration requires (seconds: number)");
                    float seconds = static_cast<float>(args[0].asNumber());
                    if (seconds < 0 || seconds > media::config::clip::kMaxDurationSeconds)
                        throw jsi::JSError(rt, "clip duration must be 0-120 seconds");
                    size_t bufferSize = static_cast<size_t>(seconds * 150000.0f);
                    if (bufferSize < media::config::clip::kDefaultBufferSize) bufferSize = media::config::clip::kDefaultBufferSize;
                    clipBuffer_.setBufferSize(bufferSize);
                    clipBuffer_.setEnabled(seconds > 0);
                    return jsi::Value(true);
                });
        case MethodId::CaptureClip:
            return jsi::Function::createFromHostFunction(rt, name, 1,
                [this, weak = weakOwner_](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
                    if (!weak.lock()) return jsi::Value::undefined();
                    if (count < 1 || !args[0].isNumber()) throw jsi::JSError(rt, "captureClip requires (seconds: number)");
                    float seconds = static_cast<float>(args[0].asNumber());
                    auto weakOwner = weakOwner_;
                    auto* orch = this;
                    auto callInvoker = callInvoker_;
                    auto constructor = rt.global().getPropertyAsFunction(rt, "Promise");
                    return constructor.callAsConstructor(rt,
                        jsi::Function::createFromHostFunction(rt, jsi::PropNameID::forAscii(rt, "ex"), 2,
                            [weakOwner, orch, seconds, callInvoker](jsi::Runtime& rt, const jsi::Value&,
                                                                     const jsi::Value* pArgs, size_t) -> jsi::Value {
                                auto resolve = std::make_shared<jsi::Function>(pArgs[0].getObject(rt).getFunction(rt));
                                auto reject = std::make_shared<jsi::Function>(pArgs[1].getObject(rt).getFunction(rt));
                                auto owner = weakOwner.lock();
                                if (!owner) {
                                    reject->call(rt, jsi::String::createFromUtf8(rt, "module destroyed"));
                                    return jsi::Value::undefined();
                                }
                                auto clipResult = orch->clipBuffer_.extractClip(seconds);
                                if (!clipResult.error.empty()) {
                                    reject->call(rt, jsi::String::createFromUtf8(rt, clipResult.error));
                                    return jsi::Value::undefined();
                                }
                                auto clipData = std::make_shared<std::vector<uint8_t>>(std::move(clipResult.data));
                                // Hold strong ref to keep module (and thus orch) alive until async callback completes
                                auto threadOwner = weakOwner.lock();
                                // Detach previous clip thread if still running — it holds
                                // a strong owner ref so the module stays alive. Blocking
                                // join here would stall the JS thread for the full I/O.
                                if (orch->clipThread_.joinable()) orch->clipThread_.detach();
                                orch->clipThread_ = std::thread([clipData, resolve, reject, callInvoker, threadOwner, orch]() {
                                    std::string filePath = media::ClipBuffer::generateFilePath();
                                    std::string error;
                                    FILE* f = fopen(filePath.c_str(), "wb");
                                    if (!f) { error = "failed to create clip file"; }
                                    else {
                                        size_t written = fwrite(clipData->data(), 1, clipData->size(), f);
                                        fclose(f);
                                        if (written != clipData->size()) error = "incomplete write";
                                    }
                                    if (!threadOwner || !orch->runtime_) return;
                                    callInvoker->invokeAsync([resolve, reject, filePath, error, threadOwner, orch]() {
                                        if (!threadOwner || !orch->runtime_) return;
                                        auto& rt = *orch->runtime_;
                                        if (error.empty()) resolve->call(rt, jsi::String::createFromUtf8(rt, filePath));
                                        else reject->call(rt, jsi::String::createFromUtf8(rt, error));
                                    });
                                });
                                return jsi::Value::undefined();
                            }));
                });
        case MethodId::SeekTo:
            return jsi::Function::createFromHostFunction(rt, name, 1,
                [this, weak = weakOwner_](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) {
                    if (!weak.lock()) return jsi::Value(false);
                    if (count < 1 || !args[0].isNumber()) throw jsi::JSError(rt, "seekTo requires (offsetSeconds: number)");
                    return seekTo(args[0].asNumber());
                });
        case MethodId::GetBufferRangeSeconds:
            return jsi::Function::createFromHostFunction(rt, name, 0,
                [this, weak = weakOwner_](jsi::Runtime&, const jsi::Value&, const jsi::Value*, size_t) {
                    if (!weak.lock()) return jsi::Value(0.0);
                    return jsi::Value(static_cast<double>(clipBuffer_.availableRangeSeconds()));
                });
        case MethodId::Unknown:
        default:
            return jsi::Value::undefined();
        }
    }

    std::vector<jsi::PropNameID> getPropertyNames(jsi::Runtime& rt) {
        std::vector<jsi::PropNameID> names;
        names.reserve(32);
        for (const char* n : {
            "feedData", "setMuted", "setGain", "getMetrics", "start", "stop", "isRunning",
            "setAudioRoute", "getAvailableAudioRoutes", "getAvailableAudioDevices",
            "getCurrentAudioRoute", "setAudioRouteCallback", "setHealthCallback",
            "getTrackInfo", "resetStream", "requestKeyFrame", "warmUp",
            "setKeyFrameNeededCallback", "pause", "resume", "isPaused",
            "getCurrentTimeUs", "getPlaybackState", "setBufferTarget",
            "setAudioFocusCallback", "setCatchupPolicy", "setStreamStatus",
            "setPlaybackRate", "setClipBufferDuration", "captureClip",
            "seekTo", "getBufferRangeSeconds"
        }) {
            names.push_back(jsi::PropNameID::forAscii(rt, n));
        }
        return names;
    }

private:
    bool ensureRunning() noexcept {
        if (session_.isRunning()) return true;
        return session_.start();
    }

    jsi::Value feedData(jsi::Runtime& rt, const jsi::Value& bufferVal) {
        if (!session_.isRunning()) return jsi::Value(false);
        jsiutil::BufferView buffer = jsiutil::extractBuffer(rt, bufferVal);
        if (!buffer) return jsi::Value(false);

        clipBuffer_.appendBytes(buffer.data, buffer.size);
        stallRecovery_.onDataReceived();

        if (timeshifting_) {
            size_t incoming = buffer.size;
            if (incoming > kMaxLivePendingBytes) {
                // Single chunk exceeds cap — keep only the tail
                livePendingBuffer_.assign(buffer.data + incoming - kMaxLivePendingBytes,
                                           buffer.data + incoming);
            } else {
                livePendingBuffer_.insert(livePendingBuffer_.end(), buffer.data, buffer.data + incoming);
                if (livePendingBuffer_.size() > kMaxLivePendingBytes) {
                    size_t excess = livePendingBuffer_.size() - kMaxLivePendingBytes;
                    livePendingBuffer_.erase(livePendingBuffer_.begin(),
                                              livePendingBuffer_.begin() + static_cast<ptrdiff_t>(excess));
                }
            }
            return jsi::Value(true);
        }

        const auto& result = demuxer_.feedData(buffer.data, buffer.size);

        if (demuxer_.hasStreamHeader() && !clipBuffer_.hasStreamHeader()) {
            const auto& hdr = demuxer_.streamHeader();
            clipBuffer_.setStreamHeader(hdr.data(), hdr.size());
        }
        if (result.newClusterCount > 0) {
            for (int i = 0; i < result.newClusterCount; ++i) clipBuffer_.onNewCluster();
        }
        for (const auto& pkt : result.videoPackets) {
            if (pkt.isKeyFrame) clipBuffer_.onKeyFrame(pkt.ptsUs);
            clipBuffer_.onBlockPts(pkt.ptsUs);
        }
        for (const auto& pkt : result.audioPackets) {
            clipBuffer_.onBlockPts(pkt.ptsUs);
        }

        dispatchPackets(result);

        // Propagate network-layer jitter as a buffer floor to both pipelines.
        // The demuxer sees feed irregularity before packet-level jitter manifests,
        // giving both buffers time to expand preemptively.
        int64_t feedJitter = demuxer_.feedJitterUs();
        if (feedJitter > 0) {
            // Scale to buffer target: 2.5× jitter (same as packet-level formula)
            int64_t floor = (feedJitter * 5) >> 1;
            session_.setNetworkJitterFloorUs(floor);
            if (videoQueue_) videoQueue_->setNetworkJitterFloorUs(floor);
        }

        if (!result.error.empty()) {
            MEDIA_LOG_W("feedData: demux error: %s", result.error.c_str());
            if (videoQueue_) videoQueue_->requestKeyFrame();
            media::VideoSurfaceRegistry::instance().requestKeyFrame();
            return jsi::Value(false);
        }
        return jsi::Value(true);
    }

    void dispatchPackets(const media::demux::DemuxResult& result) noexcept {
        for (const auto& pkt : result.audioPackets) {
            session_.pushAudioFrame(pkt.data, pkt.size, pkt.ptsUs, pkt.durationUs);
        }
        if (videoQueue_) {
            for (const auto& pkt : result.videoPackets) {
                videoQueue_->pushEncodedFrame(pkt.data, pkt.size, pkt.ptsUs, pkt.isKeyFrame);
            }
        }
    }

    jsi::Value getMetrics(jsi::Runtime& rt) {
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
        quality.setProperty(rt, "plcFrames", static_cast<double>(metrics.plcFrames));
        quality.setProperty(rt, "peakConsecutivePLC", static_cast<int>(metrics.peakConsecutivePLC));
        result.setProperty(rt, "quality", std::move(quality));

        jsi::Object session(rt);
        session.setProperty(rt, "uptimeMs", static_cast<double>(metrics.uptimeUs / 1000));
        session.setProperty(rt, "samplesOutput", static_cast<double>(metrics.totalSamplesOutput));
        session.setProperty(rt, "playbackRate", static_cast<double>(session_.playbackRate()));
        result.setProperty(rt, "session", std::move(session));

        jsi::Object latency(rt);
        latency.setProperty(rt, "mode", media::latencyModeToString(metrics.grantedLatencyMode));
        latency.setProperty(rt, "isLowestLatency", metrics.grantedLatencyMode == media::LatencyMode::LowLatency);
        result.setProperty(rt, "latency", std::move(latency));

        jsi::Object pools(rt);
        pools.setProperty(rt, "decodedUnderPressure", metrics.decodedPoolUnderPressure);
        pools.setProperty(rt, "encodedUnderPressure", metrics.encodedPoolUnderPressure);
        result.setProperty(rt, "pools", std::move(pools));

        jsi::Object audioOutput(rt);
        audioOutput.setProperty(rt, "restartCount", static_cast<int>(metrics.audioRestartCount));
        audioOutput.setProperty(rt, "lastError", metrics.audioLastError);
        audioOutput.setProperty(rt, "actualSampleRate", metrics.actualSampleRate);
        audioOutput.setProperty(rt, "latencyUs", static_cast<double>(metrics.audioOutputLatencyUs));
        audioOutput.setProperty(rt, "callbackJitterUs", static_cast<double>(metrics.callbackJitterUs));
        audioOutput.setProperty(rt, "running", metrics.audioOutputRunning);
        audioOutput.setProperty(rt, "interrupted", metrics.interrupted);
        audioOutput.setProperty(rt, "sampleRateValid", metrics.sampleRateValid);
        result.setProperty(rt, "audioOutput", std::move(audioOutput));

        jsi::Object bluetooth(rt);
        bluetooth.setProperty(rt, "route", media::audioRouteToString(metrics.currentRoute));
        bluetooth.setProperty(rt, "isA2dp", metrics.currentRoute == media::AudioRoute::BluetoothA2dp);
        result.setProperty(rt, "bluetooth", std::move(bluetooth));

        jsi::Object jitterObj(rt);
        jitterObj.setProperty(rt, "jitterUs", static_cast<double>(metrics.jitterUs));
        jitterObj.setProperty(rt, "bufferTargetUs", static_cast<double>(metrics.bufferTargetUs));
        result.setProperty(rt, "jitter", std::move(jitterObj));

        jsi::Object drift(rt);
        drift.setProperty(rt, "driftPpm", metrics.driftPpm);
        drift.setProperty(rt, "active", metrics.driftCompensationActive);
        drift.setProperty(rt, "currentRatio", static_cast<double>(metrics.driftCurrentRatio));
        drift.setProperty(rt, "catchupRatio", static_cast<double>(metrics.catchupCurrentRatio));
        result.setProperty(rt, "drift", std::move(drift));

        jsi::Object pipeline(rt);
        pipeline.setProperty(rt, "audioStreamState", static_cast<int>(metrics.audioStreamState));
        pipeline.setProperty(rt, "currentGain", static_cast<double>(metrics.currentGain));
        pipeline.setProperty(rt, "bufferedDurationUs", static_cast<double>(metrics.bufferedDurationUs));
        pipeline.setProperty(rt, "decodedDurationUs", static_cast<double>(metrics.decodedDurationUs));
        result.setProperty(rt, "pipeline", std::move(pipeline));

        jsi::Object levels(rt);
        levels.setProperty(rt, "peakLevel", static_cast<double>(metrics.peakLevel));
        levels.setProperty(rt, "rmsLevel", static_cast<double>(metrics.rmsLevel));
        levels.setProperty(rt, "peakDbfs", static_cast<double>(metrics.peakDbfs));
        levels.setProperty(rt, "rmsDbfs", static_cast<double>(metrics.rmsDbfs));
        levels.setProperty(rt, "clipCount", static_cast<double>(metrics.clipCount));
        result.setProperty(rt, "levels", std::move(levels));

        jsi::Object stall(rt);
        auto sm = stallRecovery_.metrics();
        stall.setProperty(rt, "state", stallStateToString(sm.currentState));
        stall.setProperty(rt, "stallCount", static_cast<int>(sm.stallCount));
        stall.setProperty(rt, "recoveryCount", static_cast<int>(sm.recoveryCount));
        stall.setProperty(rt, "keyFrameRequests", static_cast<int>(sm.keyFrameRequests));
        stall.setProperty(rt, "totalStallMs", static_cast<double>(sm.totalStallDurationUs) / 1000.0);
        stall.setProperty(rt, "lastStallMs", static_cast<double>(sm.lastStallDurationUs) / 1000.0);
        stall.setProperty(rt, "longestStallMs", static_cast<double>(sm.longestStallUs) / 1000.0);
        result.setProperty(rt, "stall", std::move(stall));

        jsi::Object demux(rt);
        auto dm = demuxer_.demuxerMetrics();
        demux.setProperty(rt, "totalBytesFed", static_cast<double>(dm.totalBytesFed));
        demux.setProperty(rt, "feedDataCalls", static_cast<double>(dm.feedDataCalls));
        demux.setProperty(rt, "audioPacketsEmitted", static_cast<double>(dm.audioPacketsEmitted));
        demux.setProperty(rt, "videoPacketsEmitted", static_cast<double>(dm.videoPacketsEmitted));
        demux.setProperty(rt, "overflowCount", static_cast<double>(dm.overflowCount));
        demux.setProperty(rt, "partialDropCount", static_cast<double>(dm.partialDropCount));
        demux.setProperty(rt, "blockStallCount", static_cast<double>(dm.blockStallCount));
        demux.setProperty(rt, "parseErrorCount", static_cast<double>(dm.parseErrorCount));
        demux.setProperty(rt, "feedJitterUs", static_cast<double>(dm.feedJitterUs));
        demux.setProperty(rt, "bufferBytes", static_cast<double>(dm.bufferBytes));
        demux.setProperty(rt, "parseState", static_cast<int>(dm.parseState));
        result.setProperty(rt, "demux", std::move(demux));

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
            video.setProperty(rt, "decodeLatencyUs", static_cast<double>(vm.decodeLatencyUs.load(std::memory_order_relaxed)));
            video.setProperty(rt, "lastDecodeTimeUs", static_cast<double>(vm.lastDecodeTimeUs.load(std::memory_order_relaxed)));
            video.setProperty(rt, "lastDecodeError", static_cast<int>(vm.lastDecodeError.load(std::memory_order_relaxed)));
            video.setProperty(rt, "queueDepth", static_cast<int>(videoQueue_->pendingFrames()));
            video.setProperty(rt, "needsKeyFrame", videoQueue_->isAwaitingKeyFrame());
            video.setProperty(rt, "keyFrameRequests", static_cast<double>(vm.keyFrameRequests.load(std::memory_order_relaxed)));
            video.setProperty(rt, "decoderState", static_cast<int>(vm.decoderState.load(std::memory_order_relaxed)));
        } else {
            video.setProperty(rt, "framesReceived", 0.0); video.setProperty(rt, "framesDecoded", 0.0);
            video.setProperty(rt, "framesDropped", 0.0); video.setProperty(rt, "decodeErrors", 0.0);
            video.setProperty(rt, "currentFps", 0.0); video.setProperty(rt, "width", 0);
            video.setProperty(rt, "height", 0); video.setProperty(rt, "jitterUs", 0.0);
            video.setProperty(rt, "bufferTargetUs", 0.0); video.setProperty(rt, "lateFrames", 0.0);
            video.setProperty(rt, "skippedFrames", 0.0); video.setProperty(rt, "decoderResets", 0.0);
            video.setProperty(rt, "surfaceLostCount", 0.0); video.setProperty(rt, "maxInterFrameGapMs", 0.0);
            video.setProperty(rt, "gapsOver50ms", 0.0); video.setProperty(rt, "gapsOver100ms", 0.0);
            video.setProperty(rt, "gapsOver500ms", 0.0); video.setProperty(rt, "driftPpm", 0.0);
            video.setProperty(rt, "decodeLatencyUs", 0.0); video.setProperty(rt, "lastDecodeTimeUs", 0.0);
            video.setProperty(rt, "lastDecodeError", 0); video.setProperty(rt, "queueDepth", 0); video.setProperty(rt, "needsKeyFrame", false); video.setProperty(rt, "keyFrameRequests", 0.0); video.setProperty(rt, "decoderState", 0);
        }
        video.setProperty(rt, "avSyncOffsetUs", syncCoordinator_
            ? static_cast<double>(syncCoordinator_->currentOffsetUs()) : 0.0);
        result.setProperty(rt, "video", std::move(video));

        jsi::Object clip(rt);
        auto cm = clipBuffer_.clipMetrics();
        clip.setProperty(rt, "enabled", cm.enabled);
        clip.setProperty(rt, "bufferCapacity", static_cast<double>(cm.bufferCapacity));
        clip.setProperty(rt, "bytesUsed", static_cast<double>(cm.bytesUsed));
        clip.setProperty(rt, "clusterCount", static_cast<double>(cm.clusterCount));
        clip.setProperty(rt, "availableSeconds", static_cast<double>(cm.availableSeconds));
        result.setProperty(rt, "clip", std::move(clip));

        return result;
    }

    jsi::Value setAudioRouteCallback(jsi::Runtime& rt, const jsi::Value& callback) {
        if (callback.isNull() || callback.isUndefined()) {
            media::AudioSessionManager::instance().setRouteCallback(nullptr);
            routeCallback_.reset();
            return jsi::Value(true);
        }
        if (!callback.isObject() || !callback.asObject(rt).isFunction(rt))
            throw jsi::JSError(rt, "setAudioRouteCallback requires a function or null");
        routeCallback_ = std::make_shared<jsi::Function>(callback.asObject(rt).asFunction(rt));
        setupAudioRouteCallback();

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
                obj.setProperty(rt, "deviceName", jsi::String::createFromUtf8(rt, availableDevices[i].deviceName));
                obj.setProperty(rt, "deviceId", jsi::String::createFromUtf8(rt, availableDevices[i].deviceId));
                devicesArray.setValueAtIndex(rt, i, std::move(obj));
            }
            event.setProperty(rt, "availableDevices", std::move(devicesArray));
            routeCallback_->call(rt, event);
        }
        return jsi::Value(true);
    }

    jsi::Value setHealthCallback(jsi::Runtime& rt, const jsi::Value& callback) {
        if (callback.isNull() || callback.isUndefined()) {
            healthCallback_.reset();
            return jsi::Value(true);
        }
        if (!callback.isObject() || !callback.asObject(rt).isFunction(rt))
            throw jsi::JSError(rt, "setHealthCallback requires a function or null");
        healthCallback_ = std::make_shared<jsi::Function>(callback.asObject(rt).asFunction(rt));
        setupHealthWatchdog();
        return jsi::Value(true);
    }

    void setupHealthWatchdog() {
        if (!healthCallback_ || !callInvoker_) return;
        auto weakOwner = weakOwner_;
        auto* orch = this;

        auto readMetrics = [weakOwner, orch]() -> media::HealthSnapshot {
            media::HealthSnapshot snap{};
            auto owner = weakOwner.lock();
            if (!owner) return snap;
            if (orch->session_.isRunning()) {
                const auto& m = orch->session_.audioMetrics();
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
            snap.decodedPoolUnderPressure = orch->session_.isDecodedPoolUnderPressure();
            snap.encodedPoolUnderPressure = orch->session_.isEncodedPoolUnderPressure();
            snap.paused = orch->session_.isPaused();
            snap.bufferedDurationUs = orch->session_.bufferedDurationUs();
            snap.decodedDurationUs = orch->session_.decodedDurationUs();
            snap.uptimeUs = orch->session_.uptimeUs();
            snap.decodeThreadDetached = orch->session_.wasDecodeThreadDetached();
            snap.audioOutputRunning = orch->session_.isAudioOutputRunning();
            if (orch->syncCoordinator_) snap.avSyncOffsetUs = orch->syncCoordinator_->currentOffsetUs();
            snap.bufferTargetUs = orch->session_.bufferTargetUs();
            snap.streamStatus = orch->session_.streamStatus();
            if (orch->videoQueue_) {
                const auto& vm = orch->videoQueue_->metrics();
                snap.videoFramesReceived = vm.framesReceived.load(std::memory_order_relaxed);
                snap.videoFramesDecoded = vm.framesDecoded.load(std::memory_order_relaxed);
                snap.videoFramesDropped = vm.framesDropped.load(std::memory_order_relaxed);
            }

            // Evaluate stall recovery state on each health cycle
            orch->stallRecovery_.evaluate();

            // Check if buffer has recovered after a stall
            if (orch->stallRecovery_.state() == media::StallState::Recovering) {
                int64_t decoded = orch->session_.decodedDurationUs();
                if (decoded >= media::config::stall::kRecoveryBufferThresholdUs) {
                    orch->stallRecovery_.onBufferSufficient();
                }
            }

            return snap;
        };

        auto onHealthChange = [weakOwner, orch](const media::HealthEvent& event) {
            auto owner = weakOwner.lock();
            if (!owner || !orch->healthCallback_ || !orch->callInvoker_ || !orch->runtime_) return;
            int statusInt = static_cast<int>(event.status);
            std::string detail(event.detail);
            auto snapshot = event.metrics;
            int streamStatusInt = static_cast<int>(snapshot.streamStatus);
            orch->callInvoker_->invokeAsync([weakOwner, orch, statusInt, detail, snapshot, streamStatusInt]() {
                auto owner = weakOwner.lock();
                if (!owner || !orch->runtime_ || !orch->healthCallback_) return;
                try {
                    auto& rt = *orch->runtime_;
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
                    orch->healthCallback_->call(rt, ev);
                } catch (const std::exception& e) {
                    MEDIA_LOG_E("PipelineOrchestrator: health callback error: %s", e.what());
                }
            });
        };

        // Wire stall recovery into health evaluation cycle
        stallRecovery_.setVideoKeyFrameRequestFn([weakOwner, orch]() {
            auto owner = weakOwner.lock();
            if (!owner) return;
            if (orch->videoQueue_) orch->videoQueue_->requestKeyFrame();
            media::VideoSurfaceRegistry::instance().requestKeyFrame();
        });

        session_.setHealthWatchdog(
            std::make_unique<media::HealthWatchdog>(std::move(readMetrics), std::move(onHealthChange)));
    }

    void setupAudioRouteCallback() {
        if (!routeCallback_ || !callInvoker_) return;
        auto weakOwner = weakOwner_;
        auto* orch = this;
        auto& mgr = media::AudioSessionManager::instance();

        mgr.setDriftResetCallback([weakOwner, orch]() {
            auto owner = weakOwner.lock();
            if (!owner) return;
            if (orch->session_.isRunning()) orch->session_.requestDriftReset();
        });

        mgr.setRouteCallback([weakOwner, orch](media::AudioRoute route) {
            auto owner = weakOwner.lock();
            if (!owner || !orch->routeCallback_ || !orch->callInvoker_ || !orch->runtime_) return;
            int routeInt = static_cast<int>(route);
            auto availableDevices = media::AudioSessionManager::instance().getAvailableAudioDevices();
            orch->callInvoker_->invokeAsync([weakOwner, orch, routeInt, availableDevices]() {
                auto owner = weakOwner.lock();
                if (!owner || !orch->runtime_ || !orch->routeCallback_) return;
                try {
                    auto& rt = *orch->runtime_;
                    jsi::Object event(rt);
                    event.setProperty(rt, "route", routeInt);
                    jsi::Array devicesArray(rt, availableDevices.size());
                    for (size_t i = 0; i < availableDevices.size(); ++i) {
                        jsi::Object obj(rt);
                        obj.setProperty(rt, "route", static_cast<int>(availableDevices[i].route));
                        obj.setProperty(rt, "deviceName", jsi::String::createFromUtf8(rt, availableDevices[i].deviceName));
                        obj.setProperty(rt, "deviceId", jsi::String::createFromUtf8(rt, availableDevices[i].deviceId));
                        devicesArray.setValueAtIndex(rt, i, std::move(obj));
                    }
                    event.setProperty(rt, "availableDevices", std::move(devicesArray));
                    orch->routeCallback_->call(rt, event);
                } catch (const std::exception& e) {
                    MEDIA_LOG_E("PipelineOrchestrator: route callback error: %s", e.what());
                }
            });
        });
    }

    jsi::Value getTrackInfo(jsi::Runtime& rt) {
        if (demuxer_.parseState() != media::demux::ParseState::Streaming) return jsi::Value::null();
        const auto& info = demuxer_.trackInfo();
        jsi::Object obj(rt);
        obj.setProperty(rt, "audioCodecId", jsi::String::createFromUtf8(rt, info.audioCodecId));
        obj.setProperty(rt, "videoCodecId", jsi::String::createFromUtf8(rt, info.videoCodecId));
        obj.setProperty(rt, "videoWidth", info.videoWidth);
        obj.setProperty(rt, "videoHeight", info.videoHeight);
        return obj;
    }

    jsi::Value resetStream(jsi::Runtime&) {
        std::lock_guard<std::mutex> lk(lifecycleMtx_);
        demuxer_.reset();
        clipBuffer_.reset();
        stallRecovery_.reset();
        session_.resetStreamState();
        session_.resetHealthWindow();
        if (videoQueue_) videoQueue_->reset();
        if (syncCoordinator_) syncCoordinator_->reset();
        return jsi::Value(true);
    }

    jsi::Value setKeyFrameNeededCallback(jsi::Runtime& rt, const jsi::Value& callback) {
        if (callback.isNull() || callback.isUndefined()) {
            keyFrameNeededCallback_.reset();
            media::VideoSurfaceRegistry::instance().setKeyFrameRequestFn(nullptr);
            return jsi::Value(true);
        }
        if (!callback.isObject() || !callback.asObject(rt).isFunction(rt))
            throw jsi::JSError(rt, "setKeyFrameNeededCallback requires a function or null");
        keyFrameNeededCallback_ = std::make_shared<jsi::Function>(callback.asObject(rt).asFunction(rt));
        setupKeyFrameNeededCallback();
        return jsi::Value(true);
    }

    void setupKeyFrameNeededCallback() {
        if (!keyFrameNeededCallback_ || !callInvoker_) return;
        auto weakOwner = weakOwner_;
        auto* orch = this;

        auto jsKeyFrameNotifier = [weakOwner, orch]() {
            auto owner = weakOwner.lock();
            if (!owner || !orch->keyFrameNeededCallback_ || !orch->callInvoker_ || !orch->runtime_) return;
            orch->callInvoker_->invokeAsync([weakOwner, orch]() {
                auto owner2 = weakOwner.lock();
                if (!owner2 || !orch->runtime_ || !orch->keyFrameNeededCallback_) return;
                try { orch->keyFrameNeededCallback_->call(*orch->runtime_); }
                catch (const std::exception& e) { MEDIA_LOG_E("PipelineOrchestrator: keyframe callback error: %s", e.what()); }
            });
        };

        media::VideoSurfaceRegistry::instance().setKeyFrameRequestFn(jsKeyFrameNotifier);

        // Stall recovery also fires keyframe requests via the same JS callback
        stallRecovery_.setKeyFrameRequestFn(jsKeyFrameNotifier);

        // Video queue overflow fires keyframe request when dropping invalidates P-frames
        if (videoQueue_) videoQueue_->setKeyFrameRequestCallback(jsKeyFrameNotifier);
    }

    jsi::Value setAudioFocusCallback(jsi::Runtime& rt, const jsi::Value& callback) {
        if (callback.isNull() || callback.isUndefined()) {
            audioFocusCallback_.reset();
            media::AudioSessionManager::instance().setAudioFocusCallback(nullptr);
            return jsi::Value(true);
        }
        if (!callback.isObject() || !callback.asObject(rt).isFunction(rt))
            throw jsi::JSError(rt, "setAudioFocusCallback requires a function or null");
        audioFocusCallback_ = std::make_shared<jsi::Function>(callback.asObject(rt).asFunction(rt));

        auto weakOwner = weakOwner_;
        auto* orch = this;
        media::AudioSessionManager::instance().setAudioFocusCallback(
            [weakOwner, orch](media::AudioFocusState state) {
                auto owner = weakOwner.lock();
                if (!owner || !orch->audioFocusCallback_ || !orch->callInvoker_ || !orch->runtime_) return;
                int stateInt = static_cast<int>(state);
                orch->callInvoker_->invokeAsync([weakOwner, orch, stateInt]() {
                    auto owner2 = weakOwner.lock();
                    if (!owner2 || !orch->runtime_ || !orch->audioFocusCallback_) return;
                    try {
                        orch->audioFocusCallback_->call(*orch->runtime_, jsi::Value(stateInt));
                    } catch (const std::exception& e) {
                        MEDIA_LOG_E("PipelineOrchestrator: audio focus callback error: %s", e.what());
                    }
                });
            });

        return jsi::Value(true);
    }

    jsi::Value seekTo(double offsetSeconds) {
        if (!session_.isRunning()) return jsi::Value(false);
        if (!clipBuffer_.isEnabled()) return jsi::Value(false);

        if (offsetSeconds >= 0) {
            demuxer_.reset();
            session_.resetStreamState();
            session_.resetHealthWindow();
            if (videoQueue_) videoQueue_->reset();
            media::VideoSurfaceRegistry::instance().requestKeyFrame();
            if (!livePendingBuffer_.empty()) {
                const auto& result = demuxer_.feedData(livePendingBuffer_.data(), livePendingBuffer_.size());
                dispatchPackets(result);
                livePendingBuffer_.clear();
            }
            return jsi::Value(true);
        }

        int64_t targetPts = session_.currentTimeUs() + static_cast<int64_t>(offsetSeconds * 1'000'000.0);
        auto clipResult = clipBuffer_.extractFromPts(targetPts);
        if (!clipResult.error.empty()) {
            MEDIA_LOG_W("seekTo: %s", clipResult.error.c_str());
            return jsi::Value(false);
        }

        timeshifting_ = true;
        livePendingBuffer_.clear();
        demuxer_.reset();
        session_.resetStreamState();
        session_.resetHealthWindow();
        if (videoQueue_) videoQueue_->reset();
        if (syncCoordinator_) syncCoordinator_->reset();

        refeedBytes(clipResult.data.data(), clipResult.data.size());

        timeshifting_ = false;
        MEDIA_LOG_I("seekTo: completed seek to %.1fs", offsetSeconds);
        return jsi::Value(true);
    }

    void refeedBytes(const uint8_t* data, size_t len) noexcept {
        constexpr size_t kChunkSize = 65536;
        while (len > 0) {
            size_t chunk = std::min(len, kChunkSize);
            const auto& result = demuxer_.feedData(data, chunk);
            if (demuxer_.hasStreamHeader() && !clipBuffer_.hasStreamHeader()) {
                const auto& hdr = demuxer_.streamHeader();
                clipBuffer_.setStreamHeader(hdr.data(), hdr.size());
            }
            dispatchPackets(result);
            data += chunk;
            len -= chunk;
        }
    }

    std::mutex lifecycleMtx_;
    media::MediaSession session_;
    media::demux::WebmDemuxer demuxer_;
    media::ClipBuffer clipBuffer_;
    media::StallRecoveryController stallRecovery_;
    media::VideoFrameQueue* videoQueue_{nullptr};
    media::AVSyncCoordinator* syncCoordinator_{nullptr};

    std::shared_ptr<facebook::react::CallInvoker> callInvoker_;
    std::shared_ptr<jsi::Function> routeCallback_;
    std::shared_ptr<jsi::Function> healthCallback_;
    std::shared_ptr<jsi::Function> keyFrameNeededCallback_;
    std::shared_ptr<jsi::Function> audioFocusCallback_;
    bool timeshifting_{false};
    static constexpr size_t kMaxLivePendingBytes = 4 * 1024 * 1024;  // 4MB cap
    std::vector<uint8_t> livePendingBuffer_;
    jsi::Runtime* runtime_{nullptr};
    WeakOwner weakOwner_;
    std::thread clipThread_;

    void joinClipThread() noexcept {
        if (clipThread_.joinable()) clipThread_.join();
    }
};

}  // namespace mediamodule
