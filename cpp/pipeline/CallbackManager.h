// Manages JS callback wiring for health, audio route, keyframe, and focus events.
// Extracted from PipelineOrchestrator to isolate callback lifecycle from pipeline control.
//
// All methods called from JS thread only (via the bound Function properties on the JSI module object).
#pragma once

#include <jsi/jsi.h>
#include <ReactCommon/CallInvoker.h>
#include <memory>
#include <mutex>
#include <functional>
#include "MediaLog.h"
#include "MediaTime.h"
#include "MediaConfig.h"
#include "AudioRouteTypes.h"
#include "HealthWatchdog.h"
#include "IngestRingBuffer.h"
#include "JsiUtils.h"
#include "StallRecoveryController.h"
#include "demux/WebmDemuxer.h"
#include "video/VideoFrameQueue.h"
#include "video/VideoSurfaceRegistry.h"

// Forward: MediaSession and AudioSessionManager are platform-specific

namespace mediamodule {

namespace jsi = facebook::jsi;

// Non-owning references to subsystems needed by callbacks.
// Session is the platform-specific media::MediaSession resolved at the caller's
// include site (one concrete type per compile unit). Previously a template
// parameter on the surrounding types; collapsed since there is only one
// instantiation per platform.
// Function getter (not weak_ptr) for the ingest ring: setClipBufferDuration
// destroys + recreates the ring while the watchdog runs on the decode thread.
// A captured weak_ptr would lock the OLD ring forever (its control block dies
// when destroyRingBuffer() drops the only strong ref, and the new ring is in a
// different control block — `lock()` would always return null after a toggle).
// The function indirection lets each evaluate() observe the orchestrator's
// current shared_ptr atomically (under lifecycleMtx_), preserving metric
// continuity across restarts. Returns null if the orchestrator is mid-mutation
// (try_lock failed) or has no ring; the watchdog skips the metric for that
// cycle and reads it next time.
using IngestRingBufferGetter = std::function<std::shared_ptr<media::IngestRingBuffer>()>;

// Staleness-getter for the ingest thread's heartbeat: same lifecycleMtx_
// try_lock protocol as getIngestRingBuffer. Returns 0 if the orchestrator
// is mid-mutation or no ingest thread is present.
using IngestHeartbeatStalenessGetter = std::function<int64_t()>;

struct CallbackContext {
    media::MediaSession& session;
    media::demux::WebmDemuxer& demuxer;
    media::StallRecoveryController& stallRecovery;
    media::VideoFrameQueue* videoQueue;
    media::AVSyncCoordinator* syncCoordinator;
    IngestRingBufferGetter getIngestRingBuffer;
    IngestHeartbeatStalenessGetter getIngestHeartbeatStalenessUs;
    std::atomic<bool>& ingestDetached;
    std::shared_ptr<facebook::react::CallInvoker> callInvoker;
    std::atomic<jsi::Runtime*>& runtime;
    std::weak_ptr<void> weakOwner;
};

class CallbackManager {
public:
    void setupHealthWatchdog(const CallbackContext& ctx) {
        if (!snapshotHealthCallback() || !ctx.callInvoker) return;
        auto weakOwner = ctx.weakOwner;
        auto* mgr = this;
        auto& session = ctx.session;
        auto& demuxer = ctx.demuxer;
        auto& stallRecovery = ctx.stallRecovery;
        auto* videoQueue = ctx.videoQueue;
        auto* syncCoordinator = ctx.syncCoordinator;
        auto getRing = ctx.getIngestRingBuffer;
        auto getIngestHeartbeatUs = ctx.getIngestHeartbeatStalenessUs;
        auto& ingestDetached = ctx.ingestDetached;

        auto readMetrics = [weakOwner, &session, &demuxer, &stallRecovery, videoQueue,
                            syncCoordinator, getRing, getIngestHeartbeatUs,
                            &ingestDetached]() -> media::HealthSnapshot {
            media::HealthSnapshot snap{};
            auto owner = weakOwner.lock();
            if (!owner) return snap;
            if (session.isRunning()) {
                const auto& m = session.audioMetrics();
                snap.underruns = m.underruns.load(std::memory_order_relaxed);
                snap.silenceCallbacks = m.silenceCallbacks.load(std::memory_order_relaxed);
                snap.framesDrained = m.framesDrained.load(std::memory_order_relaxed);
                snap.fastPathSwitches = m.fastPathSwitches.load(std::memory_order_relaxed);
                snap.decodeErrors = m.decodeErrors.load(std::memory_order_relaxed);
                snap.framesDropped = m.framesDropped.load(std::memory_order_relaxed);
                snap.ptsDiscontinuities = m.ptsDiscontinuities.load(std::memory_order_relaxed);
                snap.decoderResets = m.decoderResets.load(std::memory_order_relaxed);
                snap.gapsOver500ms = m.gapsOver500ms.load(std::memory_order_relaxed);
                snap.gapsOver100ms = m.gapsOver100ms.load(std::memory_order_relaxed);
                snap.gapsOver50ms = m.gapsOver50ms.load(std::memory_order_relaxed);
                snap.audioFramesReceived = m.framesReceived.load(std::memory_order_relaxed);
                snap.audioSamplesOutput = m.samplesOutput.load(std::memory_order_relaxed);
            }
            // Pull the current ring shared_ptr through the orchestrator's
            // try_lock-protected accessor. The returned shared_ptr pins the
            // storage for this evaluate cycle; if try_lock fails (JS thread
            // mid-restart), getter returns null and we skip the metric for
            // this cycle — next evaluate (~500us) retries.
            if (getRing) {
                if (auto ring = getRing()) {
                    snap.ingestRingWriteRejects = ring->writeRejects();
                }
            }
            // Demuxer parse-error visibility: triage cannot otherwise see a
            // wedged demuxer (parseState == Error && !streamHeaderAvailable)
            // from the push-based health callback. Read the cumulative count
            // and the live state so classify() can transition to Stalled.
            auto dm = demuxer.demuxerMetrics();
            snap.parseErrorCount = dm.cumulativeParseErrorCount;
            snap.parseState = static_cast<uint8_t>(dm.parseState);
            snap.demuxerWedged = (dm.parseState == media::demux::ParseState::Error)
                                  && !dm.streamHeaderAvailable;
            snap.timeInErrorMs = dm.timeInErrorMs;
            snap.decodedPoolUnderPressure = session.isDecodedPoolUnderPressure();
            snap.encodedPoolUnderPressure = session.isEncodedPoolUnderPressure();
            snap.paused = session.isPaused();
            snap.bufferedDurationUs = session.bufferedDurationUs();
            snap.decodedDurationUs = session.decodedDurationUs();
            snap.uptimeUs = session.uptimeUs();
            snap.decodeThreadDetached = session.wasDecodeThreadDetached();
            snap.audioOutputRunning = session.isAudioOutputRunning();
            if (syncCoordinator) {
                snap.avSyncOffsetUs = syncCoordinator->currentOffsetUs();
                snap.peakAbsAvSyncOffsetUs = syncCoordinator->peakAbsOffsetUs();
                snap.avSyncExceedCount = syncCoordinator->syncExceedCount();
            }
            snap.bufferTargetUs = session.bufferTargetUs();
            snap.streamStatus = session.streamStatus();
            snap.speculativeMode = session.arrivalConfidence()
                                   >= media::config::speculative::kConfidenceThreshold;
            if (videoQueue) {
                const auto& vm = videoQueue->metrics();
                snap.videoFramesReceived = vm.framesReceived.load(std::memory_order_relaxed);
                snap.videoFramesDecoded = vm.framesDecoded.load(std::memory_order_relaxed);
                snap.videoFramesDropped = vm.framesDropped.load(std::memory_order_relaxed);
                snap.needsKeyFrame = vm.needsKeyFrame.load(std::memory_order_relaxed);
                snap.videoDecodeErrors = vm.decodeErrors.load(std::memory_order_relaxed);
                snap.videoDecoderResets = vm.decoderResets.load(std::memory_order_relaxed);
                snap.videoDecoderState = vm.decoderState.load(std::memory_order_relaxed);
                snap.videoDecodeThreadDetached = !vm.decodeThreadResponsive.load(std::memory_order_relaxed)
                    && vm.lastHeartbeatUs.load(std::memory_order_relaxed) > 0;
                // currentFps with the same decay rule MetricsCollector applies — a hung
                // decoder that stops ticking the FpsCounter would otherwise leave the
                // metric pinned at 30 forever.
                int reportedFps = vm.currentFps.load(std::memory_order_relaxed);
                int64_t lastDecode = vm.lastDecodeTimeUs.load(std::memory_order_relaxed);
                if (lastDecode > 0 && (media::nowUs() - lastDecode) > 1'000'000) reportedFps = 0;
                snap.currentFps = reportedFps;
                int64_t vhb = vm.lastHeartbeatUs.load(std::memory_order_relaxed);
                snap.timeSinceVideoHeartbeatMs = vhb > 0 ? (media::nowUs() - vhb) / 1000 : 0;
            }
            snap.ingestThreadDetached = ingestDetached.load(std::memory_order_relaxed);
            if (getIngestHeartbeatUs) {
                snap.timeSinceIngestHeartbeatMs = getIngestHeartbeatUs() / 1000;
            }

            stallRecovery.evaluate();

            if (stallRecovery.state() == media::StallState::Recovering) {
                int64_t decoded = session.decodedDurationUs();
                if (decoded >= media::config::stall::kRecoveryBufferThresholdUs) {
                    stallRecovery.onBufferSufficient();
                }
            }

            return snap;
        };

        auto callInvoker = ctx.callInvoker;
        auto& runtime = ctx.runtime;
        auto onHealthChange = [weakOwner, mgr, callInvoker, &runtime](const media::HealthEvent& event) {
            auto owner = weakOwner.lock();
            if (!owner || !callInvoker || !runtime.load(std::memory_order_acquire)) return;
            // Snapshot the callback shared_ptr under the mutex so teardown cannot
            // reset it mid-read (fixes P0-1 / torn non-atomic shared_ptr).
            auto cb = mgr->snapshotHealthCallback();
            if (!cb) return;
            int statusInt = static_cast<int>(event.status);
            std::string detail(event.detail);
            auto snapshot = event.metrics;
            int streamStatusInt = static_cast<int>(snapshot.streamStatus);
            callInvoker->invokeAsync([weakOwner, cb, statusInt, detail, snapshot, streamStatusInt, &runtime]() {
                auto owner = weakOwner.lock();
                auto* rtPtr = runtime.load(std::memory_order_acquire);
                if (!owner || !rtPtr) return;
                try {
                    auto& rt = *rtPtr;
                    jsi::Object ev(rt);
                    ev.setProperty(rt, "status", statusInt);
                    // detail is constexpr ASCII from HealthWatchdog::transitionDetail();
                    // createFromAscii skips the UTF-8 validation pass.
                    ev.setProperty(rt, "detail",
                        jsi::String::createFromAscii(rt, detail.data(), detail.size()));
                    ev.setProperty(rt, "streamStatus", streamStatusInt);
                    jsi::Object metricsObj(rt);
                    metricsObj.setProperty(rt, "underruns", static_cast<double>(snapshot.underruns));
                    metricsObj.setProperty(rt, "decodeErrors", static_cast<double>(snapshot.decodeErrors));
                    metricsObj.setProperty(rt, "framesDropped", static_cast<double>(snapshot.framesDropped));
                    metricsObj.setProperty(rt, "avSyncOffsetUs", static_cast<double>(snapshot.avSyncOffsetUs));
                    metricsObj.setProperty(rt, "bufferTargetUs", static_cast<double>(snapshot.bufferTargetUs));
                    metricsObj.setProperty(rt, "gapsOver50ms", static_cast<double>(snapshot.gapsOver50ms));
                    metricsObj.setProperty(rt, "gapsOver100ms", static_cast<double>(snapshot.gapsOver100ms));
                    metricsObj.setProperty(rt, "gapsOver500ms", static_cast<double>(snapshot.gapsOver500ms));
                    metricsObj.setProperty(rt, "ingestRingWriteRejects", static_cast<double>(snapshot.ingestRingWriteRejects));
                    metricsObj.setProperty(rt, "ptsDiscontinuities", static_cast<double>(snapshot.ptsDiscontinuities));
                    metricsObj.setProperty(rt, "decoderResets", static_cast<double>(snapshot.decoderResets));
                    metricsObj.setProperty(rt, "videoFramesReceived", static_cast<double>(snapshot.videoFramesReceived));
                    metricsObj.setProperty(rt, "videoFramesDecoded", static_cast<double>(snapshot.videoFramesDecoded));
                    metricsObj.setProperty(rt, "videoFramesDropped", static_cast<double>(snapshot.videoFramesDropped));
                    metricsObj.setProperty(rt, "needsKeyFrame", jsi::Value(snapshot.needsKeyFrame));
                    metricsObj.setProperty(rt, "videoDecodeErrors", static_cast<double>(snapshot.videoDecodeErrors));
                    metricsObj.setProperty(rt, "videoDecoderResets", static_cast<double>(snapshot.videoDecoderResets));
                    metricsObj.setProperty(rt, "videoDecoderState", static_cast<int>(snapshot.videoDecoderState));
                    metricsObj.setProperty(rt, "ingestThreadDetached", jsi::Value(snapshot.ingestThreadDetached));
                    metricsObj.setProperty(rt, "videoDecodeThreadDetached", jsi::Value(snapshot.videoDecodeThreadDetached));
                    metricsObj.setProperty(rt, "audioFramesReceived", static_cast<double>(snapshot.audioFramesReceived));
                    metricsObj.setProperty(rt, "parseErrorCount", static_cast<double>(snapshot.parseErrorCount));
                    metricsObj.setProperty(rt, "timeInErrorMs", static_cast<double>(snapshot.timeInErrorMs));
                    metricsObj.setProperty(rt, "demuxerWedged", jsi::Value(snapshot.demuxerWedged));
                    metricsObj.setProperty(rt, "silenceCallbacks", static_cast<double>(snapshot.silenceCallbacks));
                    metricsObj.setProperty(rt, "framesDrained", static_cast<double>(snapshot.framesDrained));
                    metricsObj.setProperty(rt, "fastPathSwitches", static_cast<double>(snapshot.fastPathSwitches));
                    metricsObj.setProperty(rt, "peakAbsAvSyncOffsetUs", static_cast<double>(snapshot.peakAbsAvSyncOffsetUs));
                    metricsObj.setProperty(rt, "avSyncExceedCount", static_cast<double>(snapshot.avSyncExceedCount));
                    metricsObj.setProperty(rt, "timeSinceIngestHeartbeatMs", static_cast<double>(snapshot.timeSinceIngestHeartbeatMs));
                    metricsObj.setProperty(rt, "timeSinceVideoHeartbeatMs", static_cast<double>(snapshot.timeSinceVideoHeartbeatMs));
                    metricsObj.setProperty(rt, "currentFps", static_cast<double>(snapshot.currentFps));
                    ev.setProperty(rt, "metrics", std::move(metricsObj));
                    cb->call(rt, ev);
                } catch (const std::exception& e) {
                    MEDIA_LOG_E("CallbackManager: health callback error: %s", e.what());
                }
            });
        };

        stallRecovery.setVideoKeyFrameRequestFn([weakOwner, videoQueue]() {
            auto owner = weakOwner.lock();
            if (!owner) return;
            // requestKeyFrame fires keyFrameRequestCallback_ (wired to JS in
            // setupKeyFrameNeededCallback); no separate VideoSurfaceRegistry
            // call needed — it would re-fire JS through the same notifier.
            if (videoQueue) videoQueue->requestKeyFrame();
        });

        session.setHealthWatchdog(
            std::make_unique<media::HealthWatchdog>(std::move(readMetrics), std::move(onHealthChange)));
    }

    void setupAudioRouteCallback(const CallbackContext& ctx) {
        if (!snapshotRouteCallback() || !ctx.callInvoker) return;
        auto weakOwner = ctx.weakOwner;
        auto* mgr = this;
        auto callInvoker = ctx.callInvoker;
        auto& runtime = ctx.runtime;

        media::AudioSessionManager::instance().setDriftResetCallback([weakOwner, &session = ctx.session]() {
            auto owner = weakOwner.lock();
            if (!owner) return;
            if (session.isRunning()) session.requestDriftReset();
        });

        media::AudioSessionManager::instance().setRouteCallback(
            [weakOwner, mgr, callInvoker, &runtime](media::AudioRoute route) {
            auto owner = weakOwner.lock();
            if (!owner || !callInvoker || !runtime.load(std::memory_order_acquire)) return;
            auto cb = mgr->snapshotRouteCallback();
            if (!cb) return;
            int routeInt = static_cast<int>(route);
            auto availableDevices = media::AudioSessionManager::instance().getAvailableAudioDevices();
            callInvoker->invokeAsync([weakOwner, cb, routeInt, availableDevices, &runtime]() {
                auto owner = weakOwner.lock();
                auto* rtPtr = runtime.load(std::memory_order_acquire);
                if (!owner || !rtPtr) return;
                try {
                    auto& rt = *rtPtr;
                    jsi::Object event(rt);
                    event.setProperty(rt, "route", routeInt);
                    event.setProperty(rt, "availableDevices",
                                      jsiutil::marshalAudioDevices(rt, availableDevices));
                    cb->call(rt, event);
                } catch (const std::exception& e) {
                    MEDIA_LOG_E("CallbackManager: route callback error: %s", e.what());
                }
            });
        });
    }

    void setupKeyFrameNeededCallback(const CallbackContext& ctx) {
        if (!snapshotKeyFrameCallback() || !ctx.callInvoker) return;
        auto weakOwner = ctx.weakOwner;
        auto* mgr = this;
        auto callInvoker = ctx.callInvoker;
        auto& runtime = ctx.runtime;

        auto jsKeyFrameNotifier = [weakOwner, mgr, callInvoker, &runtime]() {
            auto owner = weakOwner.lock();
            if (!owner || !callInvoker || !runtime.load(std::memory_order_acquire)) return;
            auto cb = mgr->snapshotKeyFrameCallback();
            if (!cb) return;
            callInvoker->invokeAsync([weakOwner, cb, &runtime]() {
                auto owner2 = weakOwner.lock();
                auto* rtPtr = runtime.load(std::memory_order_acquire);
                if (!owner2 || !rtPtr) return;
                try { cb->call(*rtPtr); }
                catch (const std::exception& e) { MEDIA_LOG_E("CallbackManager: keyframe callback error: %s", e.what()); }
            });
        };

        media::VideoSurfaceRegistry::instance().setKeyFrameRequestFn(jsKeyFrameNotifier);
        ctx.stallRecovery.setKeyFrameRequestFn(jsKeyFrameNotifier);
        if (ctx.videoQueue) ctx.videoQueue->setKeyFrameRequestCallback(jsKeyFrameNotifier);
    }

    jsi::Value setAudioFocusCallback(jsi::Runtime& rt, const jsi::Value& callback,
                                      const CallbackContext& ctx) {
        if (callback.isNull() || callback.isUndefined()) {
            {
                std::lock_guard<std::mutex> lk(callbackMtx_);
                audioFocusCallback_.reset();
            }
            media::AudioSessionManager::instance().setAudioFocusCallback(nullptr);
            return jsi::Value(true);
        }
        if (!callback.isObject() || !callback.asObject(rt).isFunction(rt))
            throw jsi::JSError(rt, "setAudioFocusCallback requires a function or null");
        {
            std::lock_guard<std::mutex> lk(callbackMtx_);
            audioFocusCallback_ = std::make_shared<jsi::Function>(callback.asObject(rt).asFunction(rt));
        }

        auto weakOwner = ctx.weakOwner;
        auto* mgr = this;
        auto callInvoker = ctx.callInvoker;
        auto& runtime = ctx.runtime;
        media::AudioSessionManager::instance().setAudioFocusCallback(
            [weakOwner, mgr, callInvoker, &runtime](media::AudioFocusState state) {
                auto owner = weakOwner.lock();
                if (!owner || !callInvoker || !runtime.load(std::memory_order_acquire)) return;
                auto cb = mgr->snapshotAudioFocusCallback();
                if (!cb) return;
                int stateInt = static_cast<int>(state);
                callInvoker->invokeAsync([weakOwner, cb, stateInt, &runtime]() {
                    auto owner2 = weakOwner.lock();
                    auto* rtPtr = runtime.load(std::memory_order_acquire);
                    if (!owner2 || !rtPtr) return;
                    try {
                        cb->call(*rtPtr, jsi::Value(stateInt));
                    } catch (const std::exception& e) {
                        MEDIA_LOG_E("CallbackManager: audio focus callback error: %s", e.what());
                    }
                });
            });

        return jsi::Value(true);
    }

    // Set/clear callback references. Returns whether the setup methods should be called.
    bool setHealthCallback(jsi::Runtime& rt, const jsi::Value& callback) {
        if (callback.isNull() || callback.isUndefined()) {
            std::lock_guard<std::mutex> lk(callbackMtx_);
            healthCallback_.reset();
            return false;
        }
        if (!callback.isObject() || !callback.asObject(rt).isFunction(rt))
            throw jsi::JSError(rt, "setHealthCallback requires a function or null");
        auto fn = std::make_shared<jsi::Function>(callback.asObject(rt).asFunction(rt));
        std::lock_guard<std::mutex> lk(callbackMtx_);
        healthCallback_ = std::move(fn);
        return true;
    }

    bool setRouteCallback(jsi::Runtime& rt, const jsi::Value& callback) {
        if (callback.isNull() || callback.isUndefined()) {
            media::AudioSessionManager::instance().setRouteCallback(nullptr);
            std::lock_guard<std::mutex> lk(callbackMtx_);
            routeCallback_.reset();
            return false;
        }
        if (!callback.isObject() || !callback.asObject(rt).isFunction(rt))
            throw jsi::JSError(rt, "setAudioRouteCallback requires a function or null");
        auto fn = std::make_shared<jsi::Function>(callback.asObject(rt).asFunction(rt));
        std::lock_guard<std::mutex> lk(callbackMtx_);
        routeCallback_ = std::move(fn);
        return true;
    }

    bool setKeyFrameNeededCallback(jsi::Runtime& rt, const jsi::Value& callback) {
        if (callback.isNull() || callback.isUndefined()) {
            media::VideoSurfaceRegistry::instance().setKeyFrameRequestFn(nullptr);
            std::lock_guard<std::mutex> lk(callbackMtx_);
            keyFrameNeededCallback_.reset();
            return false;
        }
        if (!callback.isObject() || !callback.asObject(rt).isFunction(rt))
            throw jsi::JSError(rt, "setKeyFrameNeededCallback requires a function or null");
        auto fn = std::make_shared<jsi::Function>(callback.asObject(rt).asFunction(rt));
        std::lock_guard<std::mutex> lk(callbackMtx_);
        keyFrameNeededCallback_ = std::move(fn);
        return true;
    }

    // Fire initial route event when callback is first set (current state notification).
    // Called from JS thread.
    void fireInitialRouteEvent(jsi::Runtime& rt) {
        auto cb = snapshotRouteCallback();
        if (!cb) return;
        auto& mgr = media::AudioSessionManager::instance();
        mgr.ensureRouteDetected();
        media::AudioRoute currentRoute = mgr.currentRoute();
        if (currentRoute == media::AudioRoute::Unknown) return;

        jsi::Object event(rt);
        event.setProperty(rt, "route", static_cast<int>(currentRoute));
        event.setProperty(rt, "availableDevices",
                          jsiutil::marshalAudioDevices(rt, mgr.getAvailableAudioDevices()));
        cb->call(rt, event);
    }

    void teardown() noexcept {
        media::AudioSessionManager::instance().setRouteCallback(nullptr);
        std::lock_guard<std::mutex> lk(callbackMtx_);
        routeCallback_.reset();
        healthCallback_.reset();
        keyFrameNeededCallback_.reset();
        audioFocusCallback_.reset();
    }

private:
    // Lock-protected snapshots: any thread can call these to get a ref-counted
    // copy of the callback while protected against concurrent reset/replace in
    // the setXxx methods above. Fixes P0-1 (non-atomic shared_ptr race).
    std::shared_ptr<jsi::Function> snapshotHealthCallback() {
        std::lock_guard<std::mutex> lk(callbackMtx_);
        return healthCallback_;
    }
    std::shared_ptr<jsi::Function> snapshotRouteCallback() {
        std::lock_guard<std::mutex> lk(callbackMtx_);
        return routeCallback_;
    }
    std::shared_ptr<jsi::Function> snapshotKeyFrameCallback() {
        std::lock_guard<std::mutex> lk(callbackMtx_);
        return keyFrameNeededCallback_;
    }
    std::shared_ptr<jsi::Function> snapshotAudioFocusCallback() {
        std::lock_guard<std::mutex> lk(callbackMtx_);
        return audioFocusCallback_;
    }

    std::mutex callbackMtx_;
    std::shared_ptr<jsi::Function> healthCallback_;
    std::shared_ptr<jsi::Function> routeCallback_;
    std::shared_ptr<jsi::Function> keyFrameNeededCallback_;
    std::shared_ptr<jsi::Function> audioFocusCallback_;
};

}  // namespace mediamodule
