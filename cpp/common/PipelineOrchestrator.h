// Shared pipeline orchestration logic for iOS and Android MediaPipelineModule.
//
// INCLUDE ORDER: Platform files MUST include MediaSession.h and AudioSessionManager.h
// BEFORE this header. These types differ per platform but share the same interface.
//
// Threading: All methods called from JS thread only (via the bound Function properties on the JSI module object).
//
// Architecture: This orchestrator delegates to extracted subsystems:
//   - BroadcastPipeline: typed stage graph connecting demux to audio/video sinks
//   - MetricsCollector: aggregates metrics from all subsystems, serializes to JSI
//   - CallbackManager: health/route/keyframe/focus callback wiring
//   - IngestThread: demux thread (uses BroadcastPipeline internally)
#pragma once

#include <jsi/jsi.h>
#include <ReactCommon/CallInvoker.h>
#include <mutex>
#include <memory>
#include <vector>
#include <string>
#include <thread>
#include "MediaLog.h"
#include "MediaConfig.h"
#include "JsiUtils.h"
#include "AudioRouteTypes.h"
#include "MediaTime.h"
#include "HealthWatchdog.h"
#include "ClipIndex.h"
#include "StallRecoveryController.h"
#include "IngestRingBuffer.h"
#include "IngestThread.h"
#include "demux/WebmDemuxer.h"
#if HAVE_WHISPER
#include "transcript/TranscriptThread.h"
#include "transcript/TranscriptRegistry.h"
#endif
#include "video/VideoFrameQueue.h"
#include "video/VideoSurfaceRegistry.h"
#include "pipeline/BroadcastPipeline.h"
#include "pipeline/MetricsCollector.h"
#include "pipeline/CallbackManager.h"
#include "JsiDispatchTable.h"

namespace mediamodule {

namespace jsi = facebook::jsi;

// All shared pipeline logic. Platform MediaPipelineModule owns one of these
// and exposes the JS surface via createApiObject() at install time — a plain
// jsi::Object with each method pre-bound as a Function property.
//
// WeakOwner is a type-erased weak_ptr to the owning module, used in
// callback closures to check liveness without coupling to the concrete type.
class PipelineOrchestrator {
public:
    using WeakOwner = std::weak_ptr<void>;

    ~PipelineOrchestrator() { teardown(); }

    void init(std::shared_ptr<facebook::react::CallInvoker> invoker,
              jsi::Runtime* rt, WeakOwner owner) noexcept {
        // Warm the clock on the JS thread so the first RT-thread nowUs()
        // never pays a function-local-static guard acquisition.
        media::primeClock();
        callInvoker_ = std::move(invoker);
        runtime_.store(rt, std::memory_order_release);
        weakOwner_ = std::move(owner);
        // Hand the owning module's weak handle to MediaSession so the
        // AudioSessionManager session-restart callback (wired in start())
        // can gate on module liveness — see MediaSessionLifecycle.inl.
        session_.setOwnerWeak(weakOwner_);
        registerMethods();
    }

    void setSyncCoordinator(media::AVSyncCoordinator* c) noexcept {
        syncCoordinator_ = c;
        session_.setSyncCoordinator(c);
    }

    void setVideoQueue(media::VideoFrameQueue* q) noexcept {
        videoQueue_ = q;
        if (q) {
            if (ingestRingBuffer_) q->setRingBuffer(ingestRingBuffer_.get());
            media::VideoSurfaceRegistry::instance().setSurfaceLostFn([q]() {
                q->metrics().surfaceLostCount.fetch_add(1, std::memory_order_relaxed);
                q->requestKeyFrame();
            });
        } else {
            media::VideoSurfaceRegistry::instance().setSurfaceLostFn(nullptr);
        }
    }

    void teardown() noexcept {
        // Null the runtime FIRST so any in-flight invokeAsync lambda scheduled
        // before we stopped producing callbacks short-circuits (fixes P0-2 /
        // P0-3 Runtime TOCTOU window). Host contract remains: uninstall BEFORE
        // destroying the Runtime; this ordering just minimises the window.
        runtime_.store(nullptr, std::memory_order_release);
#if HAVE_WHISPER
        media::transcript::TranscriptRegistry::instance().clearCallback(
            media::transcript::CallbackSlot::JsCallback);
        transcriptJsCallback_.reset();
        session_.setTranscriptBuffer(nullptr);
        if (transcriptThread_) {
            session_.waitForTranscriptDrain();
        }
        transcriptThread_.reset();
#endif
        stopIngestThread();
        session_.stop();
        destroyRingBuffer();
        joinClipThread();
        callbacks_.teardown();
        media::VideoSurfaceRegistry::instance().setKeyFrameRequestFn(nullptr);
        media::VideoSurfaceRegistry::instance().setSurfaceLostFn(nullptr);
    }

    media::MediaSession& session() noexcept { return session_; }
    media::demux::WebmDemuxer& demuxer() noexcept { return demuxer_; }

    // Cross-thread accessor for the current ingest ring buffer. Returns a
    // shared_ptr that pins the storage for the caller's scope. Used by the
    // health watchdog (decode thread) to read writeRejects without UAF risk
    // when the JS thread destroys/recreates the ring during a clip-mode
    // toggle. try_to_lock skips this evaluation if JS thread is mid-restart;
    // the next evaluate (~500us later) retries, so the metric resumes once
    // setClipBufferDuration completes. Avoids priority inversion.
    [[nodiscard]] std::shared_ptr<media::IngestRingBuffer> tryGetIngestRingBuffer() noexcept {
        std::unique_lock<std::mutex> lk(lifecycleMtx_, std::try_to_lock);
        if (!lk.owns_lock()) return nullptr;
        return ingestRingBuffer_;
    }

    // Build the JS-facing API object: a plain jsi::Object with each
    // registered method pre-bound as a Function property. JS accesses
    // resolve through Hermes' native property hashtable — no per-call
    // prop.utf8 alloc, no Function recreation. Call once at install time.
    jsi::Object createApiObject(jsi::Runtime& rt) {
        return dispatch_.buildApiObject(rt, weakOwner_);
    }

private:
    // ── Method registration (called once from init) ──
    //
    // Each cluster is defined out-of-class in a dedicated `bindings/*.inl` file
    // (included at the bottom of this header). Splitting by user-facing API
    // group keeps each unit small enough to read in one screen and isolates
    // changes — adding a transcription method only touches TranscriptBindings.

    void registerMethods() {
        registerLifecycleMethods();
        registerAudioControlMethods();
        registerAudioRoutingMethods();
        registerCallbackMethods();
        registerQueryMethods();
        registerClipMethods();
        registerTranscriptMethods();
    }

    void registerLifecycleMethods();
    void registerAudioControlMethods();
    void registerAudioRoutingMethods();
    void registerCallbackMethods();
    void registerQueryMethods();
    void registerClipMethods();
    void registerTranscriptMethods();

    // ── Context builders for extracted subsystems ──

    CallbackContext callbackContext() noexcept {
        // The orchestrator instance outlives the lambda's last call: the
        // owning module's weakOwner.lock() inside readMetrics gates access,
        // and the orchestrator is a member of the module, so the `this`
        // capture is valid whenever the lock succeeds.
        auto* self = this;
        return {session_, demuxer_, stallRecovery_, videoQueue_, syncCoordinator_,
                [self]() -> std::shared_ptr<media::IngestRingBuffer> {
                    return self->tryGetIngestRingBuffer();
                },
                [self]() -> int64_t {
                    // try_lock keeps us from blocking the decode thread on a
                    // JS-thread setClipBufferDuration that is rebuilding the
                    // ingest thread. Skip the metric for one cycle on miss.
                    std::unique_lock<std::mutex> lk(self->lifecycleMtx_, std::try_to_lock);
                    if (!lk.owns_lock() || !self->ingestThread_) return 0;
                    return self->ingestThread_->timeSinceLastHeartbeatUs();
                },
                ingestDetached_, callInvoker_, runtime_, weakOwner_};
    }

    MetricsContext metricsContext() noexcept {
        return {session_, demuxer_, clipIndex_, stallRecovery_, videoQueue_,
                syncCoordinator_, ingestRingBuffer_.get(), ingestThread_.get(),
                ingestBytesDropped_, ingestDetached_};
    }

    // ── Lifecycle ──

    bool ensureRunning() noexcept {
        if (session_.isRunning()) return true;
        // If a previous run had to force-detach the ingest thread (e.g. stuck JS
        // thread during teardown), refuse to reuse this module — the leaked state
        // means we can't guarantee fresh queue ordering. Caller must re-install.
        if (ingestDetached_.load(std::memory_order_acquire)) {
            MEDIA_LOG_E("ensureRunning: previous session left ingest thread detached; reinstall required");
            return false;
        }
        if (!session_.start()) return false;
        startIngestThread();
        return true;
    }

    // Defined in OrchestratorLifecycle.inl
    void startIngestThread() noexcept;
    bool stopIngestThread() noexcept;

    void destroyRingBuffer() noexcept {
        ingestRingBuffer_.reset();
    }

    // ── Data flow ──

    jsi::Value feedData(jsi::Runtime& rt, const jsi::Value& bufferVal) {
        if (!session_.isRunning()) return jsi::Value(false);
        jsiutil::BufferView buffer = jsiutil::extractBuffer(rt, bufferVal);
        if (!buffer) return jsi::Value(false);

        // Defensive: ensureRunning() creates both ingest members atomically with session start,
        // but if allocation of either failed and the session state wasn't rolled back, we could
        // still observe isRunning()=true with null pointers. Fail soft rather than crash.
        if (UNLIKELY(!ingestThread_ || !ingestRingBuffer_)) return jsi::Value(false);

        if (UNLIKELY(ingestThread_->consumeRecoveryNeeded())) {
            performDemuxerRecovery();
        }
        if (!ingestRingBuffer_->write(buffer.data, buffer.size)) {
            // Dropped write: do NOT mark "data received" — a sustained full-ring
            // condition should remain visible to the stall watchdog.
            MEDIA_LOG_W("feedData: ingest ring full, dropping %zu bytes", buffer.size);
            ingestBytesDropped_.fetch_add(buffer.size, std::memory_order_relaxed);
            media::VideoSurfaceRegistry::instance().requestKeyFrame();
            return jsi::Value(false);
        }
        stallRecovery_.onDataReceived();
        ingestThread_->wake();
        return jsi::Value(true);
    }

    // Defined in OrchestratorLifecycle.inl
    void performDemuxerRecovery() noexcept;

    // ── Stream reset & seek ──

    // Defined in OrchestratorLifecycle.inl
    jsi::Value resetStream(jsi::Runtime& rt);
    jsi::Value seekTo(double offsetSeconds);

    void joinClipThread() noexcept {
        if (clipThread_.joinable()) clipThread_.join();
    }

    // ── State ──

    std::mutex lifecycleMtx_;
    media::MediaSession session_;
    media::demux::WebmDemuxer demuxer_;
    media::ClipIndex clipIndex_;
    // shared_ptr (not unique_ptr) so the HealthWatchdog readMetrics lambda can
    // hold a weak_ptr that locks safely across setClipBufferDuration restarts.
    // The watchdog runs on the decode thread; the JS thread may destroy the
    // ring while the watchdog is mid-evaluate, and a weak_ptr lock keeps the
    // storage alive for the duration of the access.
    std::shared_ptr<media::IngestRingBuffer> ingestRingBuffer_;
    std::unique_ptr<media::pipeline::BroadcastPipeline> pipeline_;
    std::unique_ptr<media::IngestThread> ingestThread_;
    // Detached-thread keepalives: when an ingest thread fails to join we cannot
    // safely free its ring buffer. Move the shared_ptr here so the storage
    // outlives the runaway thread, mirroring the prior unique_ptr::release()
    // leak semantics. Bounded growth: detach is a one-shot fatal event per
    // module instance (ensureRunning() refuses re-init after).
    std::vector<std::shared_ptr<media::IngestRingBuffer>> detachedIngestRings_;
    std::atomic<uint64_t> ingestBytesDropped_{0};
    std::atomic<bool> ingestDetached_{false};
    media::StallRecoveryController stallRecovery_;
    media::VideoFrameQueue* videoQueue_{nullptr};
    media::AVSyncCoordinator* syncCoordinator_{nullptr};

    std::shared_ptr<facebook::react::CallInvoker> callInvoker_;
    CallbackManager callbacks_;
    JsiDispatchTable dispatch_;

    std::atomic<jsi::Runtime*> runtime_{nullptr};
    WeakOwner weakOwner_;
    std::thread clipThread_;
    std::atomic<bool> clipThreadDone_{true};

#if HAVE_WHISPER
    std::unique_ptr<media::transcript::TranscriptThread> transcriptThread_;
    std::shared_ptr<jsi::Function> transcriptJsCallback_;
#endif
};

}  // namespace mediamodule

// Heavy lifecycle implementations live next to the class. JSI binding
// definitions are split by API cluster — each .inl reopens the mediamodule
// namespace and defines one private member function out-of-class (`inline`
// for ODR across iOS + Android translation units).
#include "OrchestratorLifecycle.inl"
#include "bindings/LifecycleBindings.inl"
#include "bindings/AudioControlBindings.inl"
#include "bindings/AudioRoutingBindings.inl"
#include "bindings/CallbackBindings.inl"
#include "bindings/QueryBindings.inl"
#include "bindings/ClipBindings.inl"
#include "bindings/TranscriptBindings.inl"
